#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "semaphore.h"
#include "defs.h"

extern int semaphore_create(int, int);
extern int semaphore_wait(int);
extern int semaphore_post(int);
extern int semaphore_destroy(int);
extern int semaphore_snapshot(int, struct semaphore_info *);

/** 创建一个由当前进程拥有的教学型计数信号量。 */
uint64
sys_semcreate(void)
{
  int initial;
  int limit;

  if(argint(0, &initial) < 0 || argint(1, &limit) < 0)
    return -1;
  return semaphore_create(initial, limit);
}

/** 消费一个许可，必要时在内核等待队列中睡眠。 */
uint64
sys_semwait(void)
{
  int handle;

  if(argint(0, &handle) < 0)
    return -1;
  return semaphore_wait(handle);
}

/** 产生一个许可，计数达到创建时上界则拒绝。 */
uint64
sys_sempost(void)
{
  int handle;

  if(argint(0, &handle) < 0)
    return -1;
  return semaphore_post(handle);
}

/** 由创建者销毁信号量，并使仍在等待的调用失败返回。 */
uint64
sys_semdestroy(void)
{
  int handle;

  if(argint(0, &handle) < 0)
    return -1;
  return semaphore_destroy(handle);
}

/**
 * 把计数、等待者与累计操作次数复制到当前进程的用户缓冲区。
 *
 * @return 成功返回 0；句柄或用户地址非法返回 -1。
 */
uint64
sys_seminfo(void)
{
  struct semaphore_info info;
  struct proc *process = myproc();
  uint64 address;
  int handle;

  if(argint(0, &handle) < 0 || argaddr(1, &address) < 0)
    return -1;
  if(semaphore_snapshot(handle, &info) < 0)
    return -1;
  if(copyout(process->pagetable, address, (char *)&info, sizeof(info)) < 0)
    return -1;
  return 0;
}
