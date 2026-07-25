#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "semaphore.h"
#include "defs.h"

#define NSEMAPHORE 16
#define SEMAPHORE_INDEX_BITS 8
#define SEMAPHORE_INDEX_MASK ((1U << SEMAPHORE_INDEX_BITS) - 1)
#define SEMAPHORE_GENERATION_MAX 0x7fffffU

/** 内核持有的信号量槽位；等待队列由 proc.chan == 本对象地址表达。 */
struct kernel_semaphore {
  struct spinlock lock;
  int allocated;
  int closing;
  int owner_pid;
  int value;
  int limit;
  int waiters;
  uint generation;
  uint successful_waits;
  uint posts;
  uint wake_calls;
};

static struct kernel_semaphore semaphores[NSEMAPHORE];

/**
 * 把公开句柄拆成槽位下标和代数。
 *
 * @param handle 用户态持有的正整数句柄。
 * @param index 接收槽位下标。
 * @param generation 接收防止陈旧句柄复用的代数。
 * @return 句柄格式合法返回 0，否则返回 -1。
 */
static int
semaphore_decode_handle(int handle, int *index, uint *generation)
{
  uint raw;
  uint tag;

  if(handle <= 0)
    return -1;
  raw = (uint)handle;
  tag = raw & SEMAPHORE_INDEX_MASK;
  if(tag == 0 || tag > NSEMAPHORE)
    return -1;
  *generation = raw >> SEMAPHORE_INDEX_BITS;
  if(*generation == 0)
    return -1;
  *index = (int)tag - 1;
  return 0;
}

/** 在槽位锁内判断句柄是否仍指向一个可操作对象。 */
static int
semaphore_matches_locked(struct kernel_semaphore *semaphore, uint generation)
{
  return semaphore->allocated && !semaphore->closing &&
         semaphore->generation == generation;
}

/**
 * 在所有等待者离开后把 closing 槽位归还给分配表。
 *
 * 销毁期间保留槽位，避免旧等待者尚未递减 waiters 时被新对象重置计数。
 */
static void
semaphore_retire_if_idle_locked(struct kernel_semaphore *semaphore)
{
  if(!semaphore->closing || semaphore->waiters != 0)
    return;
  semaphore->allocated = 0;
  semaphore->closing = 0;
  semaphore->owner_pid = 0;
  semaphore->value = 0;
  semaphore->limit = 0;
}

/**
 * 使一个活动槽位失效，并唤醒仍在其 channel 上睡眠的进程。
 *
 * @param semaphore 已持有其 lock 的活动槽位。
 */
static void
semaphore_close_locked(struct kernel_semaphore *semaphore)
{
  semaphore->closing = 1;
  if(semaphore->waiters > 0){
    semaphore->wake_calls++;
    wakeup(semaphore);
  }
  semaphore_retire_if_idle_locked(semaphore);
}

/** 初始化固定大小的教学型信号量表。 */
void
seminit(void)
{
  for(int i = 0; i < NSEMAPHORE; i++)
    initlock(&semaphores[i].lock, "semaphore");
}

/**
 * 创建一个由当前进程拥有的计数信号量。
 *
 * @param initial 初始可用许可数，必须位于 [0, limit]。
 * @param limit 计数上界，必须为正数。
 * @return 成功返回带代数的正句柄；参数非法或槽位耗尽返回 -1。
 */
int
semaphore_create(int initial, int limit)
{
  struct proc *owner = myproc();

  if(initial < 0 || limit <= 0 || initial > limit)
    return -1;

  for(int i = 0; i < NSEMAPHORE; i++){
    struct kernel_semaphore *semaphore = &semaphores[i];
    int handle;

    acquire(&semaphore->lock);
    if(semaphore->allocated){
      release(&semaphore->lock);
      continue;
    }

    semaphore->generation++;
    if(semaphore->generation == 0 ||
       semaphore->generation > SEMAPHORE_GENERATION_MAX)
      semaphore->generation = 1;
    semaphore->allocated = 1;
    semaphore->closing = 0;
    semaphore->owner_pid = owner->pid;
    semaphore->value = initial;
    semaphore->limit = limit;
    semaphore->waiters = 0;
    semaphore->successful_waits = 0;
    semaphore->posts = 0;
    semaphore->wake_calls = 0;
    handle = (int)((semaphore->generation << SEMAPHORE_INDEX_BITS) |
                   (uint)(i + 1));
    release(&semaphore->lock);
    return handle;
  }

  return -1;
}

/**
 * 消费一个许可；许可为零时原子释放槽位锁并在对象地址上睡眠。
 *
 * @param handle 活动信号量句柄。
 * @return 成功消费许可返回 0；句柄失效、对象被销毁或进程被杀死返回 -1。
 */
int
semaphore_wait(int handle)
{
  struct kernel_semaphore *semaphore;
  struct proc *process = myproc();
  int index;
  uint generation;

  if(semaphore_decode_handle(handle, &index, &generation) < 0)
    return -1;
  semaphore = &semaphores[index];

  acquire(&semaphore->lock);
  for(;;){
    if(!semaphore_matches_locked(semaphore, generation)){
      semaphore_retire_if_idle_locked(semaphore);
      release(&semaphore->lock);
      return -1;
    }
    if(process->killed){
      release(&semaphore->lock);
      return -1;
    }
    if(semaphore->value > 0){
      semaphore->value--;
      semaphore->successful_waits++;
      release(&semaphore->lock);
      return 0;
    }

    semaphore->waiters++;
    sleep(semaphore, &semaphore->lock);
    semaphore->waiters--;
    if(semaphore->closing){
      semaphore_retire_if_idle_locked(semaphore);
      release(&semaphore->lock);
      return -1;
    }
  }
}

/**
 * 产生一个许可，并在存在等待者时唤醒该 channel 上的竞争者。
 *
 * @param handle 活动信号量句柄。
 * @return 成功返回 0；句柄失效或计数已达到上界返回 -1，且计数保持不变。
 */
int
semaphore_post(int handle)
{
  struct kernel_semaphore *semaphore;
  int index;
  uint generation;

  if(semaphore_decode_handle(handle, &index, &generation) < 0)
    return -1;
  semaphore = &semaphores[index];

  acquire(&semaphore->lock);
  if(!semaphore_matches_locked(semaphore, generation) ||
     semaphore->value >= semaphore->limit){
    release(&semaphore->lock);
    return -1;
  }

  semaphore->value++;
  semaphore->posts++;
  if(semaphore->waiters > 0){
    semaphore->wake_calls++;
    wakeup(semaphore);
  }
  release(&semaphore->lock);
  return 0;
}

/**
 * 由创建者销毁信号量；等待者被唤醒并以失败返回。
 *
 * @param handle 活动信号量句柄。
 * @return 创建者成功发起销毁返回 0；非创建者或陈旧句柄返回 -1。
 */
int
semaphore_destroy(int handle)
{
  struct kernel_semaphore *semaphore;
  int index;
  uint generation;

  if(semaphore_decode_handle(handle, &index, &generation) < 0)
    return -1;
  semaphore = &semaphores[index];

  acquire(&semaphore->lock);
  if(!semaphore_matches_locked(semaphore, generation) ||
     semaphore->owner_pid != myproc()->pid){
    release(&semaphore->lock);
    return -1;
  }
  semaphore_close_locked(semaphore);
  release(&semaphore->lock);
  return 0;
}

/**
 * 复制一个活动信号量的只读状态。
 *
 * @param handle 活动信号量句柄。
 * @param info 接收计数、等待者和累计操作次数。
 * @return 成功返回 0；陈旧或正在销毁的句柄返回 -1。
 */
int
semaphore_snapshot(int handle, struct semaphore_info *info)
{
  struct kernel_semaphore *semaphore;
  int index;
  uint generation;

  if(semaphore_decode_handle(handle, &index, &generation) < 0)
    return -1;
  semaphore = &semaphores[index];

  acquire(&semaphore->lock);
  if(!semaphore_matches_locked(semaphore, generation)){
    release(&semaphore->lock);
    return -1;
  }
  info->handle = handle;
  info->owner_pid = semaphore->owner_pid;
  info->value = semaphore->value;
  info->limit = semaphore->limit;
  info->waiters = semaphore->waiters;
  info->successful_waits = semaphore->successful_waits;
  info->posts = semaphore->posts;
  info->wake_calls = semaphore->wake_calls;
  release(&semaphore->lock);
  return 0;
}

/**
 * 清理指定进程创建但未显式销毁的全部信号量。
 *
 * @param owner_pid 正在退出的创建者 PID。
 */
void
semaphore_cleanup_owner(int owner_pid)
{
  for(int i = 0; i < NSEMAPHORE; i++){
    struct kernel_semaphore *semaphore = &semaphores[i];

    acquire(&semaphore->lock);
    if(semaphore->allocated && !semaphore->closing &&
       semaphore->owner_pid == owner_pid)
      semaphore_close_locked(semaphore);
    release(&semaphore->lock);
  }
}
