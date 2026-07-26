#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "fsinspect.h"
#include "file.h"
#include "proc.h"

/**
 * 将文件系统快照复制到用户态；fd=-1 只返回全局状态。
 *
 * 普通文件和设备描述符会附带 inode 身份、链接计数、大小以及直接块到一级间接块
 * 的边界映射。pipe 和无效描述符不具备 inode 语义，返回 -1 且不修改文件系统。
 */
uint64
sys_fsinspect(void)
{
  int fd;
  uint64 user_addr;
  struct inode *ip = 0;
  struct fsinspect_snapshot snapshot;
  struct proc *p = myproc();

  if(argint(0, &fd) < 0 || argaddr(1, &user_addr) < 0)
    return -1;

  if(fd != FSINSPECT_GLOBAL_FD){
    if(fd < 0 || fd >= NOFILE)
      return -1;
    struct file *f = p->ofile[fd];
    if(f == 0 || (f->type != FD_INODE && f->type != FD_DEVICE))
      return -1;
    ip = f->ip;
    ilock(ip);
  }

  fsinspect_collect(ip, &snapshot);

  if(ip != 0)
    iunlock(ip);

  if(copyout(p->pagetable, user_addr, (char*)&snapshot, sizeof(snapshot)) < 0)
    return -1;
  return 0;
}
