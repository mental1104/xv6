#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "disktrace_abi.h"
#include "defs.h"

static struct spinlock disktrace_sys_lock;
static struct disktrace_snapshot disktrace_sys_snapshot;

/** 在首个用户进程启动前初始化 disktrace read 使用的静态快照锁。 */
void
disktrace_sys_init(void)
{
  initlock(&disktrace_sys_lock, "disktracesys");
}

/**
 * sys_disktrace 控制和读取 xv6 virtio 驱动边界轨迹。
 *
 * 参数 0 是 DISKTRACE_OP_*；READ 时参数 1 为用户态 snapshot 地址，参数 2 为
 * 调用者事件容量。其余操作忽略后两个参数。轨迹只证明驱动提交、avail 发布、
 * used 完成和调用者返回，不暴露 QEMU 设备内部调度。
 *
 * @return 成功返回 0；未知操作、容量越界或 copyout 失败时返回 -1。
 */
uint64
sys_disktrace(void)
{
  int op;
  int max_events;
  uint64 address;
  struct proc *p = myproc();

  if(argint(0, &op) < 0 ||
     argaddr(1, &address) < 0 ||
     argint(2, &max_events) < 0)
    return -1;

  switch(op){
  case DISKTRACE_OP_RESET:
    virtio_disk_trace_reset();
    return 0;
  case DISKTRACE_OP_START:
    return virtio_disk_trace_start();
  case DISKTRACE_OP_STOP:
    return virtio_disk_trace_stop();
  case DISKTRACE_OP_READ:
    acquire(&disktrace_sys_lock);
    if(virtio_disk_trace_copy_snapshot(&disktrace_sys_snapshot,
                                       max_events) < 0){
      release(&disktrace_sys_lock);
      return -1;
    }
    if(copyout(p->pagetable, address, (char *)&disktrace_sys_snapshot,
               sizeof(disktrace_sys_snapshot)) < 0){
      release(&disktrace_sys_lock);
      return -1;
    }
    release(&disktrace_sys_lock);
    return 0;
  default:
    return -1;
  }
}
