//
// File descriptors
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

/** 初始化全局打开文件表。 */
void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

/** 分配一个带引用的文件表项。 */
struct file*
filealloc(void)
{
  acquire(&ftable.lock);
  for(struct file *f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

/** 复制一个打开文件引用。 */
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

/** 释放打开文件引用，并在最后一个引用关闭时释放底层对象。 */
void
fileclose(struct file *f)
{
  struct file ff;

  // raw console 所有权同时绑定 PID 与 file 对象；必须在 ftable 失效前回收。
  if(f->type == FD_DEVICE && f->major == CONSOLE)
    consolefileclose(f, myproc()->pid);

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

/** 把 inode 元数据复制到用户态 stat。 */
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;

  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(copyout(p->pagetable, addr, (char*)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

/** 从文件读取并推进 64 位打开文件偏移。 */
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(!f->readable)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }
  return r;
}

/**
 * 写入文件并推进 64 位打开文件偏移。
 *
 * 每个分片都小于日志预留上限。磁盘满时允许返回短写：已经完成的字节被提交
 * 并返回给用户，而不是因为 short write 触发 panic。
 */
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(!f->writable)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
    int i = 0;

    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      r = writei(f->ip, 1, addr + i, f->off, n1);
      if(r > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r <= 0)
        break;
      i += r;
      if(r != n1)
        break;
    }

    if(i == n)
      ret = n;
    else if(i > 0)
      ret = i;
    else
      ret = -1;
  } else {
    panic("filewrite");
  }

  return ret;
}
