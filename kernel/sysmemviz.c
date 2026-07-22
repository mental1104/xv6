#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "memviz.h"
#include "defs.h"

/**
 * sys_memsnapshot 将当前进程可观察到的内存状态复制到用户空间。
 *
 * 系统调用参数 0 为 MEMVIZ_VIEW_*，参数 1 为用户态输出结构体地址。
 * 采样函数返回前已经释放 allocator 锁，因此 copyout 不在锁内执行。
 *
 * @return 成功返回 0；参数、view 或 copyout 无效时返回 -1。
 */
uint64
sys_memsnapshot(void)
{
  int view;
  uint64 address;

  if(argint(0, &view) < 0 || argaddr(1, &address) < 0)
    return -1;

  struct memviz_snapshot snapshot;
  if(memviz_snapshot(view, &snapshot) < 0)
    return -1;

  struct proc *p = myproc();
  if(copyout(p->pagetable, address, (char *)&snapshot, sizeof(snapshot)) < 0)
    return -1;
  return 0;
}
