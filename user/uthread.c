#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/uthread.h"

#define PREEMPT_MIN_ITERATIONS 1000000ULL
#define PREEMPT_TIMEOUT_TICKS 20
#define LIFECYCLE_ROUNDS 3
#define STACK_CONTEXT_ROUNDS 4
#define STACK_CONTEXT_STEP 7

static volatile uint64 preempt_progress[2];
static volatile uint64 preempt_result[2];
static volatile uint64 preempt_iterations[2];
static volatile int preempt_saw_peer[2];
static volatile int capacity_seen[UTHREAD_MAX_WORKERS];
static volatile int lifecycle_seen[LIFECYCLE_ROUNDS][UTHREAD_MAX_WORKERS];
static volatile int shared_address_value;
static volatile int stack_ready[2];

/** 验证共享地址空间、借用参数和 join 失败语义所需的跨线程观察值。 */
struct api_contract_argument {
  int expected;
  int observed;
  int worker_pid;
  int self_tid;
  int self_join_result;
};

/** 记录一个工作线程独立栈上的局部变量地址和切换前后值。 */
struct stack_context_argument {
  int id;
  uint64 stack_address;
  int before;
  int after;
};

/**
 * 验证线程入口收到的是调用者持有的参数指针，并与主线程共享进程状态。
 *
 * @param argument 指向主线程栈上的 api_contract_argument；主线程会在线程真正
 *                 运行前修改其字段，且一直保留到 join 完成。
 * @return 无；观察结果写回 argument 和 shared_address_value。
 */
static void
api_contract_worker(void *argument)
{
  struct api_contract_argument *item = argument;

  item->observed = item->expected;
  item->worker_pid = getpid();
  shared_address_value = item->expected + 1;
  item->self_join_result = thread_join(item->self_tid);
}

/**
 * 在独立工作栈上保留局部哨兵，并跨多次主动切换验证执行上下文不被覆盖。
 *
 * @param argument 指向主线程栈上的 stack_context_argument，id 必须为 0 或 1。
 * @return 无；局部变量地址和切换前后值写回 argument。
 */
static void
stack_context_worker(void *argument)
{
  struct stack_context_argument *item = argument;
  int sentinel = 1000 + item->id;

  item->stack_address = (uint64)&sentinel;
  item->before = sentinel;
  stack_ready[item->id] = 1;

  // 两个线程都建立局部栈帧后再继续，避免顺序执行也能误过“独立栈”检查。
  while(stack_ready[1 - item->id] == 0)
    thread_yield();

  for(int round = 0; round < STACK_CONTEXT_ROUNDS; round++){
    sentinel += STACK_CONTEXT_STEP;
    thread_yield();
  }
  item->after = sentinel;
}

/**
 * 验证共享地址空间、参数借用、self/duplicate/invalid join 与回收语义。
 *
 * 该测试在线程抢占启动前执行，因此 thread_create() 返回后、thread_join() 切换
 * 前，主线程可以确定性地修改参数对象。工作线程必须观察到修改后的值，证明
 * 运行时保存的是借用指针而非参数副本；相同 PID 和共享变量写入则证明它不是
 * fork/wait 进程模型。
 *
 * @return 全部 API 契约成立返回 0；创建、观察、join 或回收异常返回 -1。
 */
static int
run_api_contract_test(void)
{
  struct api_contract_argument argument = {
    .expected = 7,
    .observed = -1,
    .worker_pid = -1,
    .self_tid = -1,
    .self_join_result = 0,
  };
  int main_pid = getpid();
  int tid;
  int duplicate_join;
  int invalid_negative;
  int invalid_main;
  int invalid_high;

  shared_address_value = 0;
  tid = thread_create(api_contract_worker, &argument);
  if(tid < 0)
    return -1;

  // 参数所有权仍在调用者；线程运行前的修改必须通过同一指针被观察到。
  argument.expected = 42;
  argument.self_tid = tid;
  if(thread_join(tid) < 0)
    return -1;

  duplicate_join = thread_join(tid);
  invalid_negative = thread_join(-1);
  invalid_main = thread_join(0);
  invalid_high = thread_join(UTHREAD_MAX_WORKERS + 1);

  if(argument.observed != 42 ||
     argument.worker_pid != main_pid ||
     shared_address_value != 43 ||
     argument.self_join_result != -1 ||
     duplicate_join != -1 ||
     invalid_negative != -1 ||
     invalid_main != -1 ||
     invalid_high != -1){
    printf("uthread: api detail observed=%d worker_pid=%d main_pid=%d shared=%d "
           "self=%d duplicate=%d invalid=%d/%d/%d\n",
           argument.observed, argument.worker_pid, main_pid,
           shared_address_value, argument.self_join_result, duplicate_join,
           invalid_negative, invalid_main, invalid_high);
    return -1;
  }
  return 0;
}

/**
 * 验证两个线程拥有不同用户栈，且局部变量可跨多次上下文切换保持。
 *
 * @return 两个栈地址非零且不同、哨兵值完整时返回 0，否则返回 -1。
 */
static int
run_stack_context_test(void)
{
  struct stack_context_argument arguments[2];
  int tids[2];

  memset((void *)stack_ready, 0, sizeof(stack_ready));
  for(int i = 0; i < 2; i++){
    arguments[i].id = i;
    arguments[i].stack_address = 0;
    arguments[i].before = -1;
    arguments[i].after = -1;
    tids[i] = thread_create(stack_context_worker, &arguments[i]);
    if(tids[i] < 0)
      return -1;
  }

  for(int i = 0; i < 2; i++){
    if(thread_join(tids[i]) < 0)
      return -1;
  }

  int expected0 = 1000 + STACK_CONTEXT_ROUNDS * STACK_CONTEXT_STEP;
  int expected1 = 1001 + STACK_CONTEXT_ROUNDS * STACK_CONTEXT_STEP;
  if(arguments[0].stack_address == 0 ||
     arguments[1].stack_address == 0 ||
     arguments[0].stack_address == arguments[1].stack_address ||
     arguments[0].before != 1000 ||
     arguments[1].before != 1001 ||
     arguments[0].after != expected0 ||
     arguments[1].after != expected1){
    printf("uthread: stack detail address0=%p address1=%p before=%d/%d "
           "after=%d/%d expected=%d/%d\n",
           arguments[0].stack_address, arguments[1].stack_address,
           arguments[0].before, arguments[1].before,
           arguments[0].after, arguments[1].after, expected0, expected1);
    return -1;
  }
  return 0;
}

/**
 * CPU 密集型抢占测试线程，不主动调用 thread_yield()。
 *
 * 每个线程至少执行一百万次运算，并继续运行到观察到另一线程已前进。这样首个
 * 线程不会在 timer tick 到达前退出，测试能够区分真正抢占和 thread_exit() 带来
 * 的顺序切换。20 tick 仅作为 alarm 失效时的失败上限，不参与正常调度。
 *
 * @param argument 取值为 0 或 1 的线程序号，通过整数到指针的转换传入。
 * @return 无；结果写入 preempt_* 全局数组后由 trampoline 自动退出。
 */
static void
preempt_worker(void *argument)
{
  uint64 id = (uint64)argument;
  uint64 peer = 1 - id;
  uint64 seed = 17 + id;
  uint64 iterations = 0;
  uint64 sum = 0;
  int start_tick = uptime();

  for(;;){
    sum += iterations + seed;
    iterations++;
    preempt_progress[id] = iterations;
    if(preempt_progress[peer] != 0)
      preempt_saw_peer[id] = 1;

    if(iterations >= PREEMPT_MIN_ITERATIONS && preempt_saw_peer[id])
      break;
    if((iterations & 0x3ffffULL) == 0 &&
       uptime() - start_tick >= PREEMPT_TIMEOUT_TICKS)
      break;
  }

  preempt_iterations[id] = iterations;
  preempt_result[id] = sum;
}

/**
 * 记录容量测试中对应工作线程已真正运行。
 *
 * @param argument 指向稳定存在的 int 下标，范围为 0 到 UTHREAD_MAX_WORKERS-1。
 * @return 无；只写入当前下标对应的独立标志位。
 */
static void
capacity_worker(void *argument)
{
  int index = *(int *)argument;
  capacity_seen[index] = 1;
}

/** 生命周期测试传给一个工作线程的轮次与槽位下标。 */
struct lifecycle_argument {
  int round;
  int index;
};

/**
 * 记录生命周期压力测试中某一轮、某一槽位对应的执行结果。
 *
 * @param argument 指向主线程栈上稳定存在的 lifecycle_argument 数组元素。
 * @return 无；只写入唯一的二维标志位，避免共享自增的数据竞争。
 */
static void
lifecycle_worker(void *argument)
{
  struct lifecycle_argument *item = argument;
  lifecycle_seen[item->round][item->index] = 1;
}

/**
 * 验证两个不主动 yield 的 CPU 密集线程会被 timer 交替抢占且计算上下文完整。
 *
 * @return 验收通过返回 0；创建、join、并发进展或寄存器相关计算异常返回 -1。
 */
static int
run_preempt_test(void)
{
  int tids[2];

  for(int i = 0; i < 2; i++){
    tids[i] = thread_create(preempt_worker, (void *)(uint64)i);
    if(tids[i] < 0)
      return -1;
  }
  for(int i = 0; i < 2; i++){
    if(thread_join(tids[i]) < 0)
      return -1;
  }

  for(uint64 id = 0; id < 2; id++){
    uint64 iterations = preempt_iterations[id];
    uint64 expected = iterations * (iterations - 1) / 2;
    expected += iterations * (17 + id);
    if(iterations < PREEMPT_MIN_ITERATIONS ||
       preempt_result[id] != expected || !preempt_saw_peer[id]){
      printf("uthread: preempt detail id=%d iterations=%d peer=%d result=%d expected=%d\n",
             id, iterations, preempt_saw_peer[id], preempt_result[id], expected);
      return -1;
    }
  }
  return 0;
}

/**
 * 验证 16 个工作线程可同时占用全部容量，第 17 次创建明确失败且已有线程完好。
 *
 * @return 容量、失败语义和执行结果全部正确返回 0，否则返回 -1；失败时打印
 *         精确阶段、线程编号和观察值，便于区分分配与回收错误。
 */
static int
run_capacity_test(void)
{
  int arguments[UTHREAD_MAX_WORKERS];
  int tids[UTHREAD_MAX_WORKERS];

  // xv6 用户 ELF 从虚拟地址 0 开始链接；首个容量工作函数也覆盖合法零地址入口回归。
  for(int i = 0; i < UTHREAD_MAX_WORKERS; i++){
    arguments[i] = i;
    tids[i] = thread_create(capacity_worker, &arguments[i]);
    if(tids[i] < 0){
      printf("uthread: capacity detail create index=%d tid=%d\n", i, tids[i]);
      return -1;
    }
  }

  int overflow_tid = thread_create(capacity_worker, &arguments[0]);
  if(overflow_tid != -1){
    printf("uthread: capacity detail overflow tid=%d\n", overflow_tid);
    return -1;
  }

  for(int i = 0; i < UTHREAD_MAX_WORKERS; i++){
    int join_result = thread_join(tids[i]);
    if(join_result < 0 || capacity_seen[i] != 1){
      printf("uthread: capacity detail join index=%d tid=%d result=%d seen=%d\n",
             i, tids[i], join_result, capacity_seen[i]);
      return -1;
    }
  }
  return 0;
}

/**
 * 连续三轮创建、运行、join 16 个线程，验证 ZOMBIE 槽位可稳定回收复用。
 *
 * @return 48 次线程执行均完成且每轮 join 成功返回 0，否则返回 -1。
 */
static int
run_lifecycle_test(void)
{
  struct lifecycle_argument arguments[UTHREAD_MAX_WORKERS];
  int tids[UTHREAD_MAX_WORKERS];

  for(int round = 0; round < LIFECYCLE_ROUNDS; round++){
    for(int i = 0; i < UTHREAD_MAX_WORKERS; i++){
      arguments[i].round = round;
      arguments[i].index = i;
      tids[i] = thread_create(lifecycle_worker, &arguments[i]);
      if(tids[i] < 0)
        return -1;
    }
    for(int i = 0; i < UTHREAD_MAX_WORKERS; i++){
      if(thread_join(tids[i]) < 0 || lifecycle_seen[round][i] != 1)
        return -1;
    }
  }
  return 0;
}

/**
 * 运行 M:1 用户线程的 API 契约、独立栈、抢占、容量和生命周期验收场景。
 *
 * @param argc 命令行参数数量，本程序忽略额外参数。
 * @param argv 命令行参数数组，本程序忽略额外参数。
 * @return 通过 exit() 返回：全部通过为 0，初始化或任一阶段失败为 1。
 */
int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  if(thread_init() < 0){
    printf("uthread: initialization failed\n");
    exit(1);
  }

  if(run_api_contract_test() < 0){
    printf("uthread: api-contract FAILED\n");
    exit(1);
  }
  printf("uthread: shared-address OK same-pid=1 shared=43\n");
  printf("uthread: argument-lifetime OK observed=42 ownership=borrowed\n");
  printf("uthread: join-failures OK self=-1 duplicate=-1 invalid=-1\n");

  if(thread_start() < 0){
    printf("uthread: preemption initialization failed\n");
    exit(1);
  }

  if(run_stack_context_test() < 0){
    printf("uthread: stack-context FAILED\n");
    exit(1);
  }
  printf("uthread: stack-context OK distinct=1 preserved=2\n");

  if(run_preempt_test() < 0){
    printf("uthread: preempt FAILED\n");
    exit(1);
  }
  printf("uthread: preempt OK\n");

  if(run_capacity_test() < 0){
    printf("uthread: capacity FAILED\n");
    exit(1);
  }
  printf("uthread: capacity OK created=%d overflow=-1\n", UTHREAD_MAX_WORKERS);

  if(run_lifecycle_test() < 0){
    printf("uthread: lifecycle FAILED\n");
    exit(1);
  }
  printf("uthread: lifecycle OK rounds=%d completed=%d\n",
         LIFECYCLE_ROUNDS, LIFECYCLE_ROUNDS * UTHREAD_MAX_WORKERS);

  sigalarm(0, 0);
  printf("uthread: all tests OK\n");
  exit(0);
}
