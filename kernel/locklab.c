#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "locklab.h"
#include "defs.h"

/**
 * 保存锁实验的共享对象和可观察状态。
 *
 * counter_lock 保护 counter；sleep_lock 表示允许持有者睡眠的长期所有权；
 * state_lock 只保护睡眠实验的短期条件状态。测试代码不能直接访问这些锁，
 * 只能通过 locklab_run() 触发受控状态转移。
 */
static struct {
  struct spinlock counter_lock;
  int counter;

  struct sleeplock sleep_lock;
  struct spinlock state_lock;
  int holder_ready;
  int waiter_started;
  int waiter_acquired;
  int release_requested;
} locklab_state;

/** 初始化锁实验的全部锁和共享状态。 */
void
locklabinit(void)
{
  initlock(&locklab_state.counter_lock, "locklab.counter");
  initsleeplock(&locklab_state.sleep_lock, "locklab.sleep");
  initlock(&locklab_state.state_lock, "locklab.state");
  locklab_state.counter = 0;
  locklab_state.holder_ready = 0;
  locklab_state.waiter_started = 0;
  locklab_state.waiter_acquired = 0;
  locklab_state.release_requested = 0;
}

/**
 * 重置计数器和睡眠实验状态。
 *
 * @return 当前没有正在执行的 holder/waiter 时返回 0；实验仍活跃时返回 -1，
 *         避免重置覆盖另一个进程正在等待的条件。
 */
static int
locklab_reset(void)
{
  acquire(&locklab_state.state_lock);
  if(locklab_state.holder_ready ||
     (locklab_state.waiter_started && !locklab_state.waiter_acquired)){
    release(&locklab_state.state_lock);
    return -1;
  }
  locklab_state.holder_ready = 0;
  locklab_state.waiter_started = 0;
  locklab_state.waiter_acquired = 0;
  locklab_state.release_requested = 0;
  release(&locklab_state.state_lock);

  acquire(&locklab_state.counter_lock);
  locklab_state.counter = 0;
  release(&locklab_state.counter_lock);
  return 0;
}

/**
 * 读取计数器的一次快照。
 *
 * @return counter_lock 临界区内观察到的计数值。
 *
 * 该操作本身没有数据竞争，但它故意只保护“读”而不保护完整读改写事务，
 * 用于稳定复现临界区粒度过小造成的丢失更新。
 */
static int
locklab_split_read(void)
{
  int value;

  acquire(&locklab_state.counter_lock);
  value = locklab_state.counter;
  release(&locklab_state.counter_lock);
  return value;
}

/**
 * 将调用者提供的快照结果写回计数器。
 *
 * @param value 要写入的整数值。
 * @return 写入成功后返回 0。
 *
 * 与 locklab_split_read() 分开调用时，两个进程可以先后读取同一个旧值，
 * 再分别写回同一个新值；每次访问都加锁仍不足以保证复合事务原子性。
 */
static int
locklab_split_write(int value)
{
  acquire(&locklab_state.counter_lock);
  locklab_state.counter = value;
  release(&locklab_state.counter_lock);
  return 0;
}

/**
 * 在一个 spinlock 临界区内完成完整读改写。
 *
 * @return 递增后的计数值。
 */
static int
locklab_safe_increment(void)
{
  int value;

  acquire(&locklab_state.counter_lock);
  locklab_state.counter++;
  value = locklab_state.counter;
  release(&locklab_state.counter_lock);
  return value;
}

/** @return counter_lock 保护下读取到的当前计数值。 */
static int
locklab_counter(void)
{
  int value;

  acquire(&locklab_state.counter_lock);
  value = locklab_state.counter;
  release(&locklab_state.counter_lock);
  return value;
}

/**
 * 验证 holding() 的所有权语义和 acquire()/release() 的中断嵌套边界。
 *
 * @return LOCKLAB_OWNER_* 位图；正常实现必须返回 LOCKLAB_OWNER_EXPECTED。
 *
 * holding() 要求本 CPU 中断已关闭，因而外部与释放后的负向检查由显式
 * push_off()/pop_off() 包围。acquire() 在这个范围内再嵌套一次 push_off()，
 * release() 只撤销自己对应的一层，不能提前恢复中断。
 */
static int
locklab_owner_oracle(void)
{
  int result = 0;
  int base_noff;

  push_off();
  base_noff = mycpu()->noff;
  if(!holding(&locklab_state.counter_lock))
    result |= LOCKLAB_OWNER_OUTSIDE_REJECTED;

  acquire(&locklab_state.counter_lock);
  if(holding(&locklab_state.counter_lock) && mycpu()->noff == base_noff + 1)
    result |= LOCKLAB_OWNER_INSIDE_HELD;
  release(&locklab_state.counter_lock);

  if(!holding(&locklab_state.counter_lock) && mycpu()->noff == base_noff)
    result |= LOCKLAB_OWNER_AFTER_REJECTED;
  pop_off();
  return result;
}

/**
 * 持有 sleeplock 后等待用户态发出释放请求。
 *
 * @return 正常被唤醒并释放 sleeplock 时返回 0。
 *
 * holder 在 sleep() 中只原子释放 state_lock；sleep_lock 会跨调度保持占有。
 * 这正是 sleeplock 与 spinlock 的责任边界：长期等待可以睡眠，但不能带着
 * spinlock 空转或进入调度器。
 */
static int
locklab_sleep_holder(void)
{
  acquiresleep(&locklab_state.sleep_lock);

  acquire(&locklab_state.state_lock);
  locklab_state.holder_ready = 1;
  while(!locklab_state.release_requested)
    sleep(&locklab_state.release_requested, &locklab_state.state_lock);
  locklab_state.holder_ready = 0;
  release(&locklab_state.state_lock);

  releasesleep(&locklab_state.sleep_lock);
  return 0;
}

/**
 * 登记等待状态并尝试获取 holder 当前占有的 sleeplock。
 *
 * @return 最终取得并释放 sleeplock 时返回 0。
 */
static int
locklab_sleep_waiter(void)
{
  acquire(&locklab_state.state_lock);
  locklab_state.waiter_started = 1;
  release(&locklab_state.state_lock);

  acquiresleep(&locklab_state.sleep_lock);
  acquire(&locklab_state.state_lock);
  locklab_state.waiter_acquired = 1;
  release(&locklab_state.state_lock);
  releasesleep(&locklab_state.sleep_lock);
  return 0;
}

/** @return state_lock 保护下生成的 LOCKLAB_SLEEP_* 状态位图。 */
static int
locklab_sleep_state(void)
{
  int state = 0;

  acquire(&locklab_state.state_lock);
  if(locklab_state.holder_ready)
    state |= LOCKLAB_SLEEP_HOLDER_READY;
  if(locklab_state.waiter_started)
    state |= LOCKLAB_SLEEP_WAITER_STARTED;
  if(locklab_state.waiter_acquired)
    state |= LOCKLAB_SLEEP_WAITER_ACQUIRED;
  if(locklab_state.release_requested)
    state |= LOCKLAB_SLEEP_RELEASE_REQUEST;
  release(&locklab_state.state_lock);
  return state;
}

/**
 * 设置 holder 的释放条件并唤醒睡在同一条件地址上的进程。
 *
 * @return holder 已经进入实验时返回 0；尚未准备好时返回 -1。
 */
static int
locklab_sleep_release(void)
{
  acquire(&locklab_state.state_lock);
  if(!locklab_state.holder_ready){
    release(&locklab_state.state_lock);
    return -1;
  }
  locklab_state.release_requested = 1;
  wakeup(&locklab_state.release_requested);
  release(&locklab_state.state_lock);
  return 0;
}

/**
 * 执行一个受控的锁实验操作。
 *
 * @param operation kernel/locklab.h 中定义的 LOCKLAB_* 操作编号。
 * @param value 仅 LOCKLAB_SPLIT_WRITE 使用的写回值；其他操作忽略。
 * @return 各操作的结果；未知操作或不满足前置状态时返回 -1。
 */
int
locklab_run(int operation, int value)
{
  switch(operation){
  case LOCKLAB_RESET:
    return locklab_reset();
  case LOCKLAB_SPLIT_READ:
    return locklab_split_read();
  case LOCKLAB_SPLIT_WRITE:
    return locklab_split_write(value);
  case LOCKLAB_SAFE_INCREMENT:
    return locklab_safe_increment();
  case LOCKLAB_COUNTER:
    return locklab_counter();
  case LOCKLAB_OWNER_ORACLE:
    return locklab_owner_oracle();
  case LOCKLAB_SLEEP_HOLDER:
    return locklab_sleep_holder();
  case LOCKLAB_SLEEP_WAITER:
    return locklab_sleep_waiter();
  case LOCKLAB_SLEEP_STATE:
    return locklab_sleep_state();
  case LOCKLAB_SLEEP_RELEASE:
    return locklab_sleep_release();
  default:
    return -1;
  }
}
