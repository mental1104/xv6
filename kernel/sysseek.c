#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

/**
 * 将一个系统调用参数解析为当前进程已打开的文件对象。
 *
 * @param n 文件描述符参数在 a0-a5 中的下标。
 * @return 描述符有效时返回借用的 file 指针；越界或未打开时返回 0。
 */
static struct file*
seekargfile(int n)
{
  int fd;
  struct proc *p = myproc();

  if(argint(n, &fd) < 0 || fd < 0 || fd >= NOFILE)
    return 0;
  return p->ofile[fd];
}

/**
 * 实现教学型 lseek(fd, offset, whence) 系统调用入口。
 *
 * offset 保留完整的 64 位有符号值；具体对象类型、负偏移和最大文件边界由
 * fileseek() 统一校验。成功返回新的文件偏移，失败返回 -1。
 */
uint64
sys_lseek(void)
{
  struct file *f;
  int64 offset;
  int whence;

  f = seekargfile(0);
  if(f == 0 || argint64(1, &offset) < 0 || argint(2, &whence) < 0)
    return -1;
  return fileseek(f, offset, whence);
}
