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
#include "fcntl.h"
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

/**
 * 释放打开文件引用，并在最后一个引用关闭时释放底层对象。
 *
 * @param f 当前进程正在关闭的共享 file 对象；调用者仍持有一个有效引用。
 *
 * 只有进程名为 `sh` 的进程可能通过 consolemode() 成为 raw mode owner。
 * 因此普通进程关闭继承的 console fd 时直接跳过 owner 检查，避免高并发
 * fork/exit 压力把所有进程串行化到 cons.lock；Shell 路径仍由 PID 与 file
 * 对象双重校验，异常退出时继续恢复 cooked mode。
 */
void
fileclose(struct file *f)
{
  struct file ff;
  struct proc *p = myproc();

  // sys_consolemode() 只允许名为 sh 的进程声明 raw ownership。该守卫既保持
  // owner 异常退出清理，又避免普通进程退出时无意义地竞争全局 console 锁。
  if(f->type == FD_DEVICE && f->major == CONSOLE && p != 0 &&
     strncmp(p->name, "sh", sizeof(p->name)) == 0)
    consolefileclose(f, p->pid);

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

/**
 * 按 SEEK_SET、SEEK_CUR 或 SEEK_END 更新普通文件的共享 64 位偏移。
 *
 * @param f 已打开的 file 对象；仅 T_FILE-backed FD_INODE 支持定位。
 * @param offset 相对 whence 基准的有符号字节偏移。
 * @param whence kernel/fcntl.h 定义的 SEEK_SET、SEEK_CUR 或 SEEK_END。
 * @return 成功返回新的非负偏移；对象不支持定位、结果为负或超过
 *         MAXFILE_BYTES 时返回 -1。
 *
 * inode 锁同时保护 inode size 和当前 file offset 的读改写，使 dup()/fork()
 * 共享 file 对象时，lseek 与普通 read/write 保持相同的串行化边界。定位本身
 * 不修改 inode size，也不分配数据块。
 */
int64
fileseek(struct file *f, int64 offset, int whence)
{
  uint64 base;
  uint64 next;

  if(f->type != FD_INODE)
    return -1;

  ilock(f->ip);
  if(f->ip->type != T_FILE)
    goto invalid;

  switch(whence){
  case SEEK_SET:
    base = 0;
    break;
  case SEEK_CUR:
    base = f->off;
    break;
  case SEEK_END:
    base = f->ip->size;
    break;
  default:
    goto invalid;
  }

  if(base > MAXFILE_BYTES)
    goto invalid;
  if(offset < 0){
    // 先偏移一再取反，避免对 INT64_MIN 直接求负产生有符号溢出。
    uint64 magnitude = (uint64)(-(offset + 1)) + 1;
    if(magnitude > base)
      goto invalid;
    next = base - magnitude;
  } else {
    if((uint64)offset > MAXFILE_BYTES - base)
      goto invalid;
    next = base + (uint64)offset;
  }

  f->off = next;
  iunlock(f->ip);
  return next;

 invalid:
  iunlock(f->ip);
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
