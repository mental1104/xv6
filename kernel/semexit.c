#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

extern void proc_exit_without_semaphore(int) __attribute__((noreturn));
extern void semaphore_cleanup_owner(int);

/**
 * 在进入既有进程退出实现前，使当前进程拥有的教学型信号量全部失效。
 *
 * Makefile 将 proc.c 中原始 exit() 重命名为 proc_exit_without_semaphore()；
 * 因此系统调用退出、kill 和 trap 失败最终都会经过本包装层，而不是只覆盖 sys_exit。
 *
 * @param status 传递给父进程 wait() 的退出状态。
 */
void
exit(int status)
{
  struct proc *process = myproc();

  semaphore_cleanup_owner(process->pid);
  proc_exit_without_semaphore(status);
}
