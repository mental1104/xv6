#include "kernel/types.h"
#include "kernel/user_context.h"
#include "user/user.h"
#include "user/uthread.h"

#define UTHREAD_SLOT_COUNT (UTHREAD_MAX_WORKERS + 1)
#define UTHREAD_STACK_SIZE 4096

/** 用户线程在运行时内的生命周期状态。 */
enum thread_state {
  THREAD_FREE = 0,
  THREAD_RUNNING,
  THREAD_RUNNABLE,
  THREAD_BLOCKED,
  THREAD_ZOMBIE,
};

/**
 * 保存一个用户线程的完整用户现场、入口参数、生命周期状态和独立静态栈。
 *
 * 槽位 0 代表调用 thread_init() 的主线程；其余 16 个槽位属于工作线程。
 * 工作线程退出后先停留在 ZOMBIE，只有 joiner 回收后才能重新成为 FREE。
 */
struct thread_slot {
  struct user_context context;
  thread_func function;
  void *argument;
  int state;
  int joiner_tid;
  char stack[UTHREAD_STACK_SIZE] __attribute__((aligned(16)));
};

static struct thread_slot threads[UTHREAD_SLOT_COUNT];
static struct user_context context_template;
static int current_tid;
static int initialized;
static int started;
static volatile int switch_guard;

/**
 * 禁止 alarm handler 进入调度器，并阻止编译器把后续状态修改移到 guard 前。
 *
 * @return 无；调用者必须在不再访问调度状态时通过 scheduler_leave() 或
 *         ucontext_switch(..., &switch_guard) 重新开放抢占。
 */
static void
scheduler_enter(void)
{
  switch_guard = 1;
  __asm__ volatile("" ::: "memory");
}

/**
 * 提交调度状态并重新开放 alarm 抢占。
 *
 * @return 无；编译器屏障保证先前的状态写入不会越过 guard 清零。
 */
static void
scheduler_leave(void)
{
  __asm__ volatile("" ::: "memory");
  switch_guard = 0;
}

/**
 * 在循环队列中查找下一个 RUNNABLE 线程。
 *
 * @param from_tid 当前线程编号，扫描从它的后继槽位开始。
 * @return 找到时返回线程编号；没有可运行线程时返回 -1。
 */
static int
find_next_runnable(int from_tid)
{
  for(int offset = 1; offset < UTHREAD_SLOT_COUNT; offset++){
    int candidate = (from_tid + offset) % UTHREAD_SLOT_COUNT;
    if(threads[candidate].state == THREAD_RUNNABLE)
      return candidate;
  }
  return -1;
}

/**
 * 将一个空闲槽位初始化为首次可运行的用户线程上下文。
 *
 * @param slot 要初始化的槽位，调用前必须处于 THREAD_FREE。
 * @param function 线程入口，调用者必须保证其可执行；地址 0 在 xv6 中可能有效。
 * @param argument 原样传给线程入口的用户指针，允许为空。
 * @return 无；函数只修改槽位元数据和首次恢复所需的寄存器快照。
 */
static void
prepare_thread(struct thread_slot *slot, thread_func function, void *argument)
{
  uint64 stack_top;

  memmove(&slot->context, &context_template, sizeof(slot->context));
  slot->function = function;
  slot->argument = argument;
  slot->joiner_tid = -1;
  slot->context.epc = 0;
  slot->context.gpr[USER_CONTEXT_RA_INDEX] = 0;
  stack_top = (uint64)(slot->stack + UTHREAD_STACK_SIZE);
  slot->context.gpr[USER_CONTEXT_SP_INDEX] = stack_top & ~((uint64)15);
  slot->context.gpr[USER_CONTEXT_A0_INDEX] = 0;
}

/**
 * 新线程首次获得 CPU 时执行用户入口，并把普通函数返回转换为线程退出。
 *
 * @return 不返回调用者；线程函数结束后切换到其他 RUNNABLE 线程。
 */
static void
thread_bootstrap(void)
{
  struct thread_slot *slot = &threads[current_tid];
  thread_func function = slot->function;
  void *argument = slot->argument;

  function(argument);
  thread_exit();
}

/**
 * alarm upcall 入口：保存真正被 timer 中断的完整上下文，并恢复下一个线程。
 *
 * @return 不通过普通 C 返回；无可切换线程时由 sigreturn() 恢复中断点，
 *         切换成功时由 ucontext_switch() 直接恢复目标线程。
 */
static void
thread_preempt(void)
{
  int old_tid;
  int next_tid;

  if(!started || switch_guard){
    sigreturn();
    return;
  }

  scheduler_enter();
  old_tid = current_tid;
  next_tid = find_next_runnable(old_tid);
  if(next_tid < 0){
    scheduler_leave();
    sigreturn();
    return;
  }

  threads[old_tid].state = THREAD_RUNNABLE;
  threads[next_tid].state = THREAD_RUNNING;
  current_tid = next_tid;
  if(ucontext_switch(&threads[old_tid].context,
                     &threads[next_tid].context,
                     &switch_guard) < 0){
    current_tid = old_tid;
    threads[old_tid].state = THREAD_RUNNING;
    threads[next_tid].state = THREAD_RUNNABLE;
    scheduler_leave();
    sigreturn();
  }

  exit(2);
}

/**
 * 初始化当前进程内的线程槽位，并保存首次线程上下文模板。
 *
 * @return 首次初始化成功返回 0；重复初始化或上下文保存失败返回 -1。
 */
int
thread_init(void)
{
  if(initialized)
    return -1;

  memset(threads, 0, sizeof(threads));
  if(ucontext_switch(&context_template, 0, 0) < 0)
    return -1;

  current_tid = 0;
  threads[0].state = THREAD_RUNNING;
  threads[0].joiner_tid = -1;
  initialized = 1;
  return 0;
}

/**
 * 从固定工作槽中分配一个线程，并准备首次恢复所需的完整用户现场。
 *
 * @param function 要执行的用户函数。xv6 用户 ELF 从虚拟地址 0 开始链接，
 *                 因此不能用 function == 0 判断入口无效。
 * @param argument 原样传入 function 的用户指针，允许为空且不转移所有权。
 * @return 成功返回 1 到 UTHREAD_MAX_WORKERS；未初始化或没有空闲槽位返回 -1。
 */
int
thread_create(thread_func function, void *argument)
{
  int tid = -1;

  if(!initialized)
    return -1;

  scheduler_enter();
  for(int candidate = 1; candidate < UTHREAD_SLOT_COUNT; candidate++){
    if(threads[candidate].state == THREAD_FREE){
      tid = candidate;
      break;
    }
  }
  if(tid >= 0){
    prepare_thread(&threads[tid], function, argument);
    threads[tid].context.epc = (uint64)thread_bootstrap;
    threads[tid].state = THREAD_RUNNABLE;
  }
  scheduler_leave();
  return tid;
}

/**
 * 注册每 tick 触发一次的 alarm handler，启用 M:1 用户线程抢占。
 *
 * @return 启动成功或已经启动返回 0；未初始化或 sigalarm 失败返回 -1。
 */
int
thread_start(void)
{
  if(!initialized)
    return -1;
  if(started)
    return 0;

  started = 1;
  if(sigalarm(1, thread_preempt) < 0){
    started = 0;
    return -1;
  }
  return 0;
}

/**
 * 主动把当前线程放回 RUNNABLE，并切换到循环队列中的下一线程。
 *
 * @return 无；未启动或没有其他 RUNNABLE 线程时保持当前线程运行。
 */
void
thread_yield(void)
{
  int old_tid;
  int next_tid;

  if(!started)
    return;

  scheduler_enter();
  old_tid = current_tid;
  next_tid = find_next_runnable(old_tid);
  if(next_tid < 0){
    scheduler_leave();
    return;
  }

  threads[old_tid].state = THREAD_RUNNABLE;
  threads[next_tid].state = THREAD_RUNNING;
  current_tid = next_tid;
  if(ucontext_switch(&threads[old_tid].context,
                     &threads[next_tid].context,
                     &switch_guard) < 0){
    current_tid = old_tid;
    threads[old_tid].state = THREAD_RUNNING;
    threads[next_tid].state = THREAD_RUNNABLE;
    scheduler_leave();
  }
}

/**
 * 阻塞当前线程直到目标工作线程退出，并把目标槽位回收到 FREE。
 *
 * @param tid 要等待的工作线程编号，不能是 0、当前线程或越界编号。
 * @return 回收成功返回 0；编号无效、重复等待或无法继续调度返回 -1。
 */
int
thread_join(int tid)
{
  int self_tid;

  if(!initialized || tid <= 0 || tid >= UTHREAD_SLOT_COUNT || tid == current_tid)
    return -1;

  for(;;){
    int next_tid;

    scheduler_enter();
    if(threads[tid].state == THREAD_FREE){
      scheduler_leave();
      return -1;
    }
    if(threads[tid].state == THREAD_ZOMBIE){
      memset(&threads[tid].context, 0, sizeof(threads[tid].context));
      threads[tid].function = 0;
      threads[tid].argument = 0;
      threads[tid].joiner_tid = -1;
      threads[tid].state = THREAD_FREE;
      scheduler_leave();
      return 0;
    }

    self_tid = current_tid;
    if(threads[tid].joiner_tid >= 0 && threads[tid].joiner_tid != self_tid){
      scheduler_leave();
      return -1;
    }
    threads[tid].joiner_tid = self_tid;
    threads[self_tid].state = THREAD_BLOCKED;
    next_tid = find_next_runnable(self_tid);
    if(next_tid < 0){
      threads[self_tid].state = THREAD_RUNNING;
      threads[tid].joiner_tid = -1;
      scheduler_leave();
      return -1;
    }

    threads[next_tid].state = THREAD_RUNNING;
    current_tid = next_tid;
    if(ucontext_switch(&threads[self_tid].context,
                       &threads[next_tid].context,
                       &switch_guard) < 0){
      current_tid = self_tid;
      threads[self_tid].state = THREAD_RUNNING;
      threads[next_tid].state = THREAD_RUNNABLE;
      threads[tid].joiner_tid = -1;
      scheduler_leave();
      return -1;
    }
  }
}

/**
 * 把当前工作线程标记为 ZOMBIE，唤醒 joiner，并切换到下一可运行线程。
 *
 * @return 不返回；主线程调用时退出进程，无法继续调度时以状态 2 退出。
 */
void
thread_exit(void)
{
  int old_tid;
  int next_tid;
  int joiner_tid;

  if(current_tid == 0)
    exit(0);

  scheduler_enter();
  old_tid = current_tid;
  joiner_tid = threads[old_tid].joiner_tid;
  threads[old_tid].state = THREAD_ZOMBIE;
  if(joiner_tid >= 0 && threads[joiner_tid].state == THREAD_BLOCKED)
    threads[joiner_tid].state = THREAD_RUNNABLE;

  next_tid = find_next_runnable(old_tid);
  if(next_tid < 0){
    scheduler_leave();
    exit(2);
  }

  threads[next_tid].state = THREAD_RUNNING;
  current_tid = next_tid;
  if(ucontext_switch(0, &threads[next_tid].context, &switch_guard) < 0){
    scheduler_leave();
    exit(2);
  }
  exit(2);
}
