#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

#define PIPESIZE 512

struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;     // number of bytes read
  uint nwrite;    // number of bytes written
  int readopen;   // read fd is still open
  int writeopen;  // write fd is still open
};

/**
 * 分配一对共享同一 pipe 状态的读写 file 对象。
 *
 * @param f0 接收可读 file 引用。
 * @param f1 接收可写 file 引用。
 * @return 全部资源分配成功返回 0；任一步失败时回滚并返回 -1。
 */
int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  if((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if((pi = (struct pipe*)kalloc()) == 0)
    goto bad;
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  initlock(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

 bad:
  if(pi)
    kfree((char*)pi);
  if(*f0)
    fileclose(*f0);
  if(*f1)
    fileclose(*f1);
  return -1;
}

/**
 * 关闭 pipe 的一个方向，并在最后一个引用消失后释放底层页。
 *
 * @param pi 被关闭 file 引用持有的 pipe。
 * @param writable 非零表示关闭写端，否则关闭读端。
 *
 * 写端关闭会让空 pipe 的下一次 read() 返回 EOF，因此它既唤醒传统阻塞读取者，
 * 也在释放 pipe 锁后通知 pollread() 等待者。通知必须发生在 pipe 锁之外，以保持
 * pollstate.lock -> pipe.lock 的固定锁序。
 */
void
pipeclose(struct pipe *pi, int writable)
{
  int free_pipe;
  int notify_readers = 0;

  acquire(&pi->lock);
  if(writable){
    pi->writeopen = 0;
    wakeup(&pi->nread);
    notify_readers = 1;
  } else {
    pi->readopen = 0;
    wakeup(&pi->nwrite);
  }
  free_pipe = pi->readopen == 0 && pi->writeopen == 0;
  release(&pi->lock);

  // 两端都关闭时已不存在合法 pollread() 读端，无需在释放前制造一次无效唤醒。
  if(notify_readers && !free_pipe)
    pollnotify();
  if(free_pipe)
    kfree((char*)pi);
}

/**
 * 将用户缓冲区写入 pipe，空间不足时睡眠等待读取方推进。
 *
 * @param pi 目标 pipe。
 * @param addr 用户缓冲区起始地址。
 * @param n 最多写入的字节数。
 * @return 实际写入字节数；读端关闭、进程被终止或首字节复制失败时返回 -1/0。
 */
int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i;
  char ch;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  for(i = 0; i < n; i++){
    while(pi->nwrite == pi->nread + PIPESIZE){  //DOC: pipewrite-full
      if(pi->readopen == 0 || pr->killed){
        release(&pi->lock);
        return -1;
      }
      wakeup(&pi->nread);
      sleep(&pi->nwrite, &pi->lock);
    }
    if(copyin(pr->pagetable, &ch, addr + i, 1) == -1)
      break;
    pi->data[pi->nwrite++ % PIPESIZE] = ch;
  }
  wakeup(&pi->nread);
  release(&pi->lock);

  // 数据状态已提交后再获取 poll 锁，避免与 pollreadfiles() 的扫描锁序反转。
  if(i > 0)
    pollnotify();
  return i;
}

/**
 * 从 pipe 复制数据到用户缓冲区，空且仍有写端时睡眠。
 *
 * @param pi 来源 pipe。
 * @param addr 用户目标缓冲区起始地址。
 * @param n 最多读取的字节数。
 * @return 实际读取字节数；写端关闭且无数据时返回 0；进程被终止时返回 -1。
 */
int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i;
  struct proc *pr = myproc();
  char ch;

  acquire(&pi->lock);
  while(pi->nread == pi->nwrite && pi->writeopen){  //DOC: pipe-empty
    if(pr->killed){
      release(&pi->lock);
      return -1;
    }
    sleep(&pi->nread, &pi->lock); //DOC: piperead-sleep
  }
  for(i = 0; i < n; i++){  //DOC: piperead-copy
    if(pi->nread == pi->nwrite)
      break;
    ch = pi->data[pi->nread++ % PIPESIZE];
    if(copyout(pr->pagetable, addr + i, &ch, 1) == -1)
      break;
  }
  wakeup(&pi->nwrite);  //DOC: piperead-wakeup
  release(&pi->lock);
  return i;
}

/**
 * 判断下一次 pipe read() 是否能够立即取得数据或 EOF。
 *
 * @param pi 由可读 file 引用持有的 pipe。
 * @return 缓冲区非空或所有写端已关闭时返回 1，否则返回 0。
 */
int
pipereadable(struct pipe *pi)
{
  int ready;

  acquire(&pi->lock);
  ready = pi->nread != pi->nwrite || pi->writeopen == 0;
  release(&pi->lock);
  return ready;
}
