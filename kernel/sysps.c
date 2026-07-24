#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "procinfo.h"
#include "defs.h"

extern struct proc proc[NPROC];
extern struct spinlock wait_lock;

/**
 * 将内核进程状态映射为稳定的用户态快照状态。
 *
 * @param state 持有目标 p->lock 时读取的内核状态。
 * @return 对应的 PROCINFO_STATE_*；未知状态返回 PROCINFO_STATE_UNKNOWN。
 */
static int
procinfo_state(enum procstate state)
{
  switch(state){
  case USED:
    return PROCINFO_STATE_USED;
  case SLEEPING:
    return PROCINFO_STATE_SLEEPING;
  case RUNNABLE:
    return PROCINFO_STATE_RUNNABLE;
  case RUNNING:
    return PROCINFO_STATE_RUNNING;
  case STOPPED:
    return PROCINFO_STATE_STOPPED;
  case ZOMBIE:
    return PROCINFO_STATE_ZOMBIE;
  case UNUSED:
  default:
    return PROCINFO_STATE_UNKNOWN;
  }
}

/**
 * 将当前进程表复制为用户态可消费的结构化快照。
 *
 * @return 成功返回已复制的非 UNUSED 条目数；容量、地址或 copyout 非法时返回 -1。
 *
 * 参数 0 是 struct procinfo 数组的用户地址，参数 1 是数组容量。读取父子关系时遵守
 * wait_lock -> p->lock 的既有顺序。每个条目先复制到内核局部变量，释放全部锁后才
 * copyout，避免用户地址错误扩大进程表临界区。
 */
uint64
sys_getprocs(void)
{
  uint64 user_entries;
  int max_entries;
  int count = 0;
  struct proc *caller = myproc();

  if(argaddr(0, &user_entries) < 0 ||
     argint(1, &max_entries) < 0 ||
     max_entries <= 0)
    return -1;
  if(max_entries > NPROC)
    max_entries = NPROC;

  for(struct proc *target = proc;
      target < &proc[NPROC] && count < max_entries;
      target++){
    struct procinfo info;
    int present = 0;

    memset(&info, 0, sizeof(info));
    acquire(&wait_lock);
    acquire(&target->lock);
    if(target->state != UNUSED){
      info.pid = target->pid;
      info.ppid = target->parent == 0 ? 0 : target->parent->pid;
      info.state = procinfo_state(target->state);
      safestrcpy(info.name, target->name, sizeof(info.name));
      present = 1;
    }
    release(&target->lock);
    release(&wait_lock);

    if(!present)
      continue;

    uint64 destination = user_entries + (uint64)count * sizeof(info);
    if(destination < user_entries ||
       copyout(caller->pagetable, destination, (char *)&info, sizeof(info)) < 0)
      return -1;
    count++;
  }

  return count;
}
