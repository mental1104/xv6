#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

/**
 * 返回一组 pipe 读端的当前就绪位图，或等待其中任一读端就绪。
 *
 * 用户传入的数组槽位与返回位图一一对应。当前教学接口只接受可读 pipe：普通文件、
 * 设备、pipe 写端和无效描述符都会整体失败，避免把“文件读取不会睡眠”偷换成通用
 * I/O 就绪语义。
 *
 * @return 成功返回非负就绪位图；参数、用户地址或描述符非法时返回 -1。
 */
uint64
sys_pollread(void)
{
  uint64 user_fds;
  int count;
  int wait;
  int fds[NOFILE];
  struct file *files[NOFILE];
  struct proc *p = myproc();

  if(argaddr(0, &user_fds) < 0 || argint(1, &count) < 0 ||
     argint(2, &wait) < 0)
    return -1;
  if(count <= 0 || count > NOFILE || (wait != 0 && wait != 1))
    return -1;
  if(copyin(p->pagetable, (char *)fds, user_fds,
            count * sizeof(fds[0])) < 0)
    return -1;

  for(int index = 0; index < count; index++){
    int fd = fds[index];

    if(fd < 0 || fd >= NOFILE)
      return -1;
    files[index] = p->ofile[fd];
    if(files[index] == 0 || files[index]->type != FD_PIPE ||
       files[index]->readable == 0)
      return -1;
  }

  return pollreadfiles(files, count, wait);
}
