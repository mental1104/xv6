#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "memviz.h"
#include "defs.h"

static struct spinlock memvizsys_lock;
static int memvizsys_lock_ready;
static struct memviz_snapshot memvizsys_snapshot;

/**
 * ensure_memvizsys_lock 初始化 memsnapshot 系统调用的静态缓冲锁。
 *
 * 该锁只保护 sys_memsnapshot 内部复用的大型快照缓冲，避免把扩展后的
 * memviz_snapshot 放在每个进程只有一页的内核栈上。首次调用发生在普通
 * 进程上下文，重复 initlock 写入同一初值不会改变后续锁语义。
 */
static void
ensure_memvizsys_lock(void)
{
  if(!memvizsys_lock_ready){
    initlock(&memvizsys_lock, "memvizsys");
    memvizsys_lock_ready = 1;
  }
}

/**
 * sys_memsnapshot 将当前进程可观察到的内存状态复制到用户空间。
 *
 * 系统调用参数 0 为 MEMVIZ_VIEW_*，参数 1 为用户态输出结构体地址。
 * 采样函数返回前已经释放 allocator 锁；函数持有的 memvizsys_lock 只保护
 * 静态快照缓冲，防止多个进程并发 memsnapshot 时互相覆盖结果。
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

  ensure_memvizsys_lock();
  acquire(&memvizsys_lock);

  if(memviz_snapshot(view, &memvizsys_snapshot) < 0){
    release(&memvizsys_lock);
    return -1;
  }

  struct proc *p = myproc();
  if(copyout(p->pagetable, address, (char *)&memvizsys_snapshot,
             sizeof(memvizsys_snapshot)) < 0){
    release(&memvizsys_lock);
    return -1;
  }

  release(&memvizsys_lock);
  return 0;
}
