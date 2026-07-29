#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

/**
 * 保护教学型 pipe 就绪等待队列。
 *
 * 这里故意使用一个全局等待通道：它足以展示“等待多个事件源并避免丢失唤醒”的
 * 核心机制，但会产生惊群，不代表 poll/epoll 等生产接口的扩展方式。
 */
static struct {
  struct spinlock lock;
} pollstate;

/** 初始化教学型 pipe 就绪等待队列。 */
void
pollinit(void)
{
  initlock(&pollstate.lock, "poll");
}

/**
 * 唤醒所有正在等待 pipe 就绪状态变化的进程。
 *
 * 调用者必须已经释放具体 pipe 的锁，保持固定的 pollstate.lock -> pipe.lock
 * 扫描顺序，避免写入/关闭路径形成反向锁序。
 */
void
pollnotify(void)
{
  acquire(&pollstate.lock);
  wakeup(&pollstate);
  release(&pollstate.lock);
}

/**
 * 等待一组可读 pipe 中至少一个进入可推进状态。
 *
 * @param files 已由系统调用层验证为可读 pipe 的 file 指针数组；所有权仍归进程文件表。
 * @param count files 中的有效元素数量，范围为 1 到 NOFILE。
 * @param wait 非零时等待任一 pipe 就绪；为零时只返回当前快照。
 * @return 按数组槽位编码的就绪位图；进程被终止时返回 -1。
 *
 * pipe 中已有数据，或所有写端已经关闭而下一次 read() 将返回 EOF，均视为可读。
 * 扫描与 sleep 共用 pollstate.lock：写入者在改变 pipe 状态后获取同一把锁并唤醒，
 * 因此事件发生在“扫描完成、真正睡眠之前”时也不会被遗漏。
 */
int
pollreadfiles(struct file **files, int count, int wait)
{
  struct proc *p = myproc();

  acquire(&pollstate.lock);
  for(;;){
    int ready = 0;

    for(int index = 0; index < count; index++)
      if(pipereadable(files[index]->pipe))
        ready |= 1 << index;

    if(ready != 0 || wait == 0){
      release(&pollstate.lock);
      return ready;
    }
    if(p->killed){
      release(&pollstate.lock);
      return -1;
    }

    sleep(&pollstate, &pollstate.lock);
  }
}
