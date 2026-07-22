#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "memviz.h"
#include "defs.h"

extern char etext[];
extern char end[];

/**
 * 根据系统调用入口保存的 user SP 推导固定一页用户栈的边界。
 *
 * @param p 当前进程；调用期间其地址空间不会被其他进程替换。
 * @param snapshot 待填写的快照，不接管其所有权。
 */
static void
fill_user_layout(struct proc *p, struct memviz_snapshot *snapshot)
{
  uint64 sp = p->trapframe->sp;
  snapshot->process_size = p->sz;
  snapshot->user_limit = PLIC;
  snapshot->image_start = 0;
  snapshot->user_sp = sp;

  // SP 位于页边界时属于其下方的栈页；减一后再向下取整可统一两种情况。
  if(sp == 0)
    return;
  uint64 stack_bottom = PGROUNDDOWN(sp - 1);
  pte_t *pte = walk(p->pagetable, stack_bottom, 0);
  if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
    return;

  uint64 stack_top = stack_bottom + PGSIZE;
  if(sp < stack_bottom || sp > stack_top || stack_bottom < PGSIZE)
    return;

  snapshot->user_stack_valid = 1;
  snapshot->stack_bottom = stack_bottom;
  snapshot->stack_top = stack_top;
  snapshot->stack_guard_start = stack_bottom - PGSIZE;
  snapshot->image_end = snapshot->stack_guard_start;
  snapshot->dynamic_start = stack_top;
  snapshot->stack_used = stack_top - sp;
  snapshot->stack_free = sp - stack_bottom;
}

/**
 * 填写当前进程运行时使用的内核页表和固定映射边界。
 *
 * @param p 当前进程；其 kstack 和 kpagetable 在进程回收前保持有效。
 * @param snapshot 待填写的快照，不接管其所有权。
 */
static void
fill_kernel_layout(struct proc *p, struct memviz_snapshot *snapshot)
{
  uint64 sp = r_sp();

  snapshot->kernel_pagetable = (uint64)p->kpagetable;
  snapshot->kernel_sp = sp;
  snapshot->kernel_stack_guard_start = p->kstack - PGSIZE;
  snapshot->kernel_stack_bottom = p->kstack;
  snapshot->kernel_stack_top = p->kstack + PGSIZE;
  if(sp >= snapshot->kernel_stack_bottom && sp <= snapshot->kernel_stack_top){
    snapshot->kernel_stack_valid = 1;
    snapshot->kernel_stack_used = snapshot->kernel_stack_top - sp;
    snapshot->kernel_stack_free = sp - snapshot->kernel_stack_bottom;
  }

  snapshot->kernel_text_start = KERNBASE;
  snapshot->kernel_text_end = (uint64)etext;
  snapshot->kernel_data_start = (uint64)etext;
  snapshot->kernel_data_end = PHYSTOP;
  snapshot->clint_start = CLINT;
  snapshot->clint_end = CLINT + 0x10000;
  snapshot->plic_start = PLIC;
  snapshot->plic_end = PLIC + 0x400000;
  snapshot->uart_start = UART0;
  snapshot->uart_end = UART0 + PGSIZE;
  snapshot->virtio_start = VIRTIO0;
  snapshot->virtio_end = VIRTIO0 + PGSIZE;
  snapshot->trampoline = TRAMPOLINE;
  snapshot->user_mirror_start = 0;
  snapshot->user_mirror_end = p->sz;
}

/**
 * memviz_snapshot 采集指定视图所需的稳定内存快照。
 *
 * @param view MEMVIZ_VIEW_* 之一。
 * @param snapshot 输出结构体，必须为可写的内核地址。
 * @return 成功返回 0；view 非法或参数为空时返回 -1。
 */
int
memviz_snapshot(int view, struct memviz_snapshot *snapshot)
{
  if(snapshot == 0)
    return -1;
  if(view != MEMVIZ_VIEW_USER && view != MEMVIZ_VIEW_PHYS &&
     view != MEMVIZ_VIEW_KERNEL)
    return -1;

  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->view = view;

  struct proc *p = myproc();
  fill_user_layout(p, snapshot);
  fill_kernel_layout(p, snapshot);

  // kalloc 负责在锁内完成 freelist 采样，返回前已经释放所有 allocator 锁。
  kalloc_mem_snapshot(snapshot);
  return 0;
}
