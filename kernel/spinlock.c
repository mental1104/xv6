// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "concurrencylab.h"
#include "defs.h"

void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void
acquire(struct spinlock *lk)
{
  push_off(); // disable interrupts to avoid deadlock.
  if(holding(lk))
    panic("acquire");

  // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Record info about lock acquisition for holding() and debugging.
  lk->cpu = mycpu();
}

// Release the lock.
void
release(struct spinlock *lk)
{
  if(!holding(lk))
    panic("release");

  lk->cpu = 0;

  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  __sync_lock_release(&lk->locked);

  pop_off();
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int
holding(struct spinlock *lk)
{
  int r;
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

void
push_off(void)
{
  int old = intr_get();

  intr_off();
  if(mycpu()->noff == 0)
    mycpu()->intena = old;
  mycpu()->noff += 1;
}

void
pop_off(void)
{
  struct cpu *c = mycpu();
  if(intr_get())
    panic("pop_off - interruptible");
  if(c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if(c->noff == 0 && c->intena)
    intr_on();
}

/*
 * 并发入门教学探针
 *
 * 该探针和自旋锁实现放在同一编译单元，只为直接复用本文件定义的互斥原语，避免
 * 修改教学内核的链接清单。它由显式系统调用会话启用，关闭后不参与正常内核路径。
 */

/**
 * 保存并发入门实验的唯一活动会话。
 *
 * control_lock 保护会话生命周期、轨迹和屏障；counter_lock 只在受保护模式中
 * 把共享计数器的完整读—改—写串行化。RACY 模式故意不使用 counter_lock，
 * 但仍使用原子单次读写避免把 C 数据竞争本身当成实验前提。
 */
static struct {
  struct spinlock control_lock;
  struct spinlock counter_lock;
  int active;
  int session;
  int mode;
  int counter;
  int started[CONCURRENCYLAB_PARTICIPANTS];
  int barrier_arrived;
  int barrier_generation;
  int completed;
  int event_order;
  struct concurrencylab_worker_snapshot workers[CONCURRENCYLAB_PARTICIPANTS];
} concurrencylab;

/** 初始化教学探针的锁和空闲会话状态。 */
void
concurrencylab_init(void)
{
  initlock(&concurrencylab.control_lock, "concurrencylab");
  initlock(&concurrencylab.counter_lock, "concurrencycounter");
  concurrencylab.active = 0;
  concurrencylab.session = 0;
}

/**
 * 判断会话编号是否仍指向当前活动实验。
 *
 * @param session 用户态在 RESET 时取得的正整数会话编号。
 * @return 当前活动会话匹配时返回 1，否则返回 0。
 *
 * 调用方必须持有 control_lock。
 */
static int
session_matches(int session)
{
  return concurrencylab.active && session > 0 && concurrencylab.session == session;
}

/**
 * 清空一次实验的可观察状态，同时保留已经初始化的两把锁。
 *
 * @param mode CONCURRENCYLAB_MODE_RACY 或 CONCURRENCYLAB_MODE_LOCKED。
 *
 * 调用方必须持有 control_lock，且已经确认没有其他活动会话。
 */
static void
reset_state(int mode)
{
  concurrencylab.mode = mode;
  concurrencylab.counter = 0;
  concurrencylab.barrier_arrived = 0;
  concurrencylab.barrier_generation++;
  concurrencylab.completed = 0;
  concurrencylab.event_order = 0;
  memset(concurrencylab.started, 0, sizeof(concurrencylab.started));
  memset(concurrencylab.workers, 0, sizeof(concurrencylab.workers));
  for(int role = 0; role < CONCURRENCYLAB_PARTICIPANTS; role++){
    concurrencylab.workers[role].role = role;
    concurrencylab.workers[role].read_cpu = -1;
    concurrencylab.workers[role].write_cpu = -1;
  }
}

/**
 * 创建一个尚未运行参与者的教学实验会话。
 *
 * @param mode 指定无锁复合更新或自旋锁保护的复合更新。
 * @return 成功返回新的正整数会话编号；模式非法或已有活动会话时返回 -1。
 */
static int
start_session(int mode)
{
  int session;

  if(mode != CONCURRENCYLAB_MODE_RACY && mode != CONCURRENCYLAB_MODE_LOCKED)
    return -1;

  acquire(&concurrencylab.control_lock);
  if(concurrencylab.active){
    release(&concurrencylab.control_lock);
    return -1;
  }

  concurrencylab.session++;
  if(concurrencylab.session <= 0)
    concurrencylab.session = 1;
  reset_state(mode);
  concurrencylab.active = 1;
  session = concurrencylab.session;
  release(&concurrencylab.control_lock);
  return session;
}

/**
 * 在 RACY 模式中等待两个参与者都完成共享值读取。
 *
 * @param session 当前活动会话编号。
 * @return 两个参与者均到达时返回 0；会话在等待期间被关闭时返回 -1。
 *
 * 调用方进入和返回时都持有 control_lock。屏障只固定“两个读取都先于任一写入”
 * 这一竞争窗口，不规定两个参与者各自的先后次序或所在 CPU。
 */
static int
wait_for_both_reads(int session)
{
  int generation = concurrencylab.barrier_generation;

  concurrencylab.barrier_arrived++;
  if(concurrencylab.barrier_arrived == CONCURRENCYLAB_PARTICIPANTS){
    concurrencylab.barrier_arrived = 0;
    concurrencylab.barrier_generation++;
    wakeup(&concurrencylab.barrier_generation);
    return 0;
  }

  while(session_matches(session) &&
        concurrencylab.barrier_generation == generation)
    sleep(&concurrencylab.barrier_generation, &concurrencylab.control_lock);

  return session_matches(session) ? 0 : -1;
}

/**
 * 执行一次被刻意拆开的无锁共享计数器更新。
 *
 * @param session 当前活动会话编号。
 * @param role 两个唯一参与者之一，取值为 0 或 1。
 * @param result 接收当前参与者完成后的结构化轨迹。
 * @return 完整执行并记录轨迹时返回 0；会话、模式、角色或生命周期非法时返回 -1。
 *
 * 两个参与者先各自读取 0，再越过屏障并分别写入 1，因此最终值稳定为 1。
 * 该路径用原子单次 load/store 排除未定义的撕裂访问，但没有把两次访问组合成
 * 原子读—改—写，正好保留 lost update 所需的竞态窗口。
 */
static int
run_racy_worker(int session, int role,
                struct concurrencylab_worker_snapshot *result)
{
  int observed;

  acquire(&concurrencylab.control_lock);
  if(!session_matches(session) ||
     concurrencylab.mode != CONCURRENCYLAB_MODE_RACY ||
     role < 0 || role >= CONCURRENCYLAB_PARTICIPANTS ||
     concurrencylab.started[role]){
    release(&concurrencylab.control_lock);
    return -1;
  }

  concurrencylab.started[role] = 1;
  observed = __atomic_load_n(&concurrencylab.counter, __ATOMIC_RELAXED);
  concurrencylab.workers[role].observed = observed;
  concurrencylab.workers[role].read_order = ++concurrencylab.event_order;
  concurrencylab.workers[role].read_cpu = cpuid();

  if(wait_for_both_reads(session) < 0){
    release(&concurrencylab.control_lock);
    return -1;
  }
  release(&concurrencylab.control_lock);

  __atomic_store_n(&concurrencylab.counter, observed + 1, __ATOMIC_RELAXED);

  acquire(&concurrencylab.control_lock);
  if(!session_matches(session) || concurrencylab.mode != CONCURRENCYLAB_MODE_RACY){
    release(&concurrencylab.control_lock);
    return -1;
  }
  concurrencylab.workers[role].written = observed + 1;
  concurrencylab.workers[role].write_order = ++concurrencylab.event_order;
  concurrencylab.workers[role].write_cpu = cpuid();
  concurrencylab.completed++;
  *result = concurrencylab.workers[role];
  release(&concurrencylab.control_lock);
  return 0;
}

/**
 * 在自旋锁保护下执行一次完整共享计数器更新。
 *
 * @param session 当前活动会话编号。
 * @param role 两个唯一参与者之一，取值为 0 或 1。
 * @param result 接收当前参与者完成后的结构化轨迹。
 * @return 完整执行并记录轨迹时返回 0；会话、模式、角色或生命周期非法时返回 -1。
 *
 * counter_lock 覆盖读取、计算和写回的完整临界区；control_lock 仅在内部记录轨迹。
 * 所有调用都按 counter_lock → control_lock 的固定顺序加锁，避免教学探针自身引入
 * 反向加锁死锁。
 */
static int
run_locked_worker(int session, int role,
                  struct concurrencylab_worker_snapshot *result)
{
  int observed;

  acquire(&concurrencylab.counter_lock);
  acquire(&concurrencylab.control_lock);
  if(!session_matches(session) ||
     concurrencylab.mode != CONCURRENCYLAB_MODE_LOCKED ||
     role < 0 || role >= CONCURRENCYLAB_PARTICIPANTS ||
     concurrencylab.started[role]){
    release(&concurrencylab.control_lock);
    release(&concurrencylab.counter_lock);
    return -1;
  }

  concurrencylab.started[role] = 1;
  observed = concurrencylab.counter;
  concurrencylab.workers[role].observed = observed;
  concurrencylab.workers[role].read_order = ++concurrencylab.event_order;
  concurrencylab.workers[role].read_cpu = cpuid();

  concurrencylab.counter = observed + 1;
  concurrencylab.workers[role].written = observed + 1;
  concurrencylab.workers[role].write_order = ++concurrencylab.event_order;
  concurrencylab.workers[role].write_cpu = cpuid();
  concurrencylab.completed++;
  *result = concurrencylab.workers[role];

  release(&concurrencylab.control_lock);
  release(&concurrencylab.counter_lock);
  return 0;
}

/**
 * 按活动会话模式分派一个参与者的读—改—写操作。
 *
 * @param session 当前活动会话编号。
 * @param role 参与者角色，必须为 0 或 1 且只能运行一次。
 * @param result 接收结构化轨迹。
 * @return 参与者成功完成返回 0，否则返回 -1。
 */
static int
run_worker(int session, int role, struct concurrencylab_worker_snapshot *result)
{
  int mode;

  acquire(&concurrencylab.control_lock);
  if(!session_matches(session)){
    release(&concurrencylab.control_lock);
    return -1;
  }
  mode = concurrencylab.mode;
  release(&concurrencylab.control_lock);

  if(mode == CONCURRENCYLAB_MODE_RACY)
    return run_racy_worker(session, role, result);
  if(mode == CONCURRENCYLAB_MODE_LOCKED)
    return run_locked_worker(session, role, result);
  return -1;
}

/**
 * 读取一个已经完成两个参与者的活动会话快照。
 *
 * @param session 当前活动会话编号。
 * @param snapshot 接收共享终值和两条参与者轨迹。
 * @return 两个参与者都完成时返回 0；过早读取或会话失效时返回 -1。
 */
static int
snapshot_session(int session, struct concurrencylab_snapshot *snapshot)
{
  acquire(&concurrencylab.control_lock);
  if(!session_matches(session) ||
     concurrencylab.completed != CONCURRENCYLAB_PARTICIPANTS){
    release(&concurrencylab.control_lock);
    return -1;
  }

  snapshot->session = concurrencylab.session;
  snapshot->mode = concurrencylab.mode;
  snapshot->configured_cpus = XV6_CPUS;
  snapshot->active = concurrencylab.active;
  snapshot->completed = concurrencylab.completed;
  snapshot->counter = __atomic_load_n(&concurrencylab.counter, __ATOMIC_RELAXED);
  for(int role = 0; role < CONCURRENCYLAB_PARTICIPANTS; role++)
    snapshot->workers[role] = concurrencylab.workers[role];
  release(&concurrencylab.control_lock);
  return 0;
}

/**
 * 关闭活动会话并唤醒可能仍停在教学屏障上的参与者。
 *
 * @param session 当前活动会话编号。
 * @return 成功关闭返回 0；会话不存在或编号过期时返回 -1。
 */
static int
close_session(int session)
{
  acquire(&concurrencylab.control_lock);
  if(!session_matches(session)){
    release(&concurrencylab.control_lock);
    return -1;
  }
  concurrencylab.active = 0;
  concurrencylab.barrier_generation++;
  wakeup(&concurrencylab.barrier_generation);
  release(&concurrencylab.control_lock);
  return 0;
}

/**
 * 暴露并发入门教学探针的会话式系统调用。
 *
 * RESET: arg0=mode，返回正 session；RUN: arg0=session、arg1=role，并复制 worker；
 * SNAPSHOT: arg0=session，并复制完整 snapshot；CLOSE: arg0=session。
 *
 * @return 各操作的成功值；参数、会话、用户地址或状态非法时返回 -1。
 */
uint64
sys_concurrencylab(void)
{
  int op;
  int arg0;
  int arg1;
  uint64 user_result;
  struct proc *p = myproc();

  argint(0, &op);
  argint(1, &arg0);
  argint(2, &arg1);
  argaddr(3, &user_result);

  if(op == CONCURRENCYLAB_OP_RESET)
    return start_session(arg0);
  if(op == CONCURRENCYLAB_OP_RUN){
    struct concurrencylab_worker_snapshot worker;
    if(run_worker(arg0, arg1, &worker) < 0)
      return -1;
    if(copyout(p->pagetable, user_result, (char *)&worker, sizeof(worker)) < 0)
      return -1;
    return 0;
  }
  if(op == CONCURRENCYLAB_OP_SNAPSHOT){
    struct concurrencylab_snapshot snapshot;
    if(snapshot_session(arg0, &snapshot) < 0)
      return -1;
    if(copyout(p->pagetable, user_result, (char *)&snapshot, sizeof(snapshot)) < 0)
      return -1;
    return 0;
  }
  if(op == CONCURRENCYLAB_OP_CLOSE)
    return close_session(arg0);
  return -1;
}
