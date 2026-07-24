#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "memviz.h"
#include "defs.h"

extern char trampoline[];
extern char uservec[];
extern char userret[];
extern char trampoline_end[];

_Static_assert(sizeof(struct trapframe) ==
               MEMVIZ_TRAPFRAME_SLOT_COUNT * sizeof(uint64),
               "memviz trapframe slots must match struct trapframe ABI");

#define ASSERT_TRAPFRAME_SLOT(member, slot) \
  _Static_assert(__builtin_offsetof(struct trapframe, member) == \
                 (slot) * sizeof(uint64), \
                 "memviz trapframe slot mismatch: " #member)

ASSERT_TRAPFRAME_SLOT(kernel_satp, MEMVIZ_TF_KERNEL_SATP);
ASSERT_TRAPFRAME_SLOT(kernel_sp, MEMVIZ_TF_KERNEL_SP);
ASSERT_TRAPFRAME_SLOT(kernel_trap, MEMVIZ_TF_KERNEL_TRAP);
ASSERT_TRAPFRAME_SLOT(epc, MEMVIZ_TF_EPC);
ASSERT_TRAPFRAME_SLOT(kernel_hartid, MEMVIZ_TF_KERNEL_HARTID);
ASSERT_TRAPFRAME_SLOT(ra, MEMVIZ_TF_RA);
ASSERT_TRAPFRAME_SLOT(sp, MEMVIZ_TF_SP);
ASSERT_TRAPFRAME_SLOT(gp, MEMVIZ_TF_GP);
ASSERT_TRAPFRAME_SLOT(tp, MEMVIZ_TF_TP);
ASSERT_TRAPFRAME_SLOT(t0, MEMVIZ_TF_T0);
ASSERT_TRAPFRAME_SLOT(t1, MEMVIZ_TF_T1);
ASSERT_TRAPFRAME_SLOT(t2, MEMVIZ_TF_T2);
ASSERT_TRAPFRAME_SLOT(s0, MEMVIZ_TF_S0);
ASSERT_TRAPFRAME_SLOT(s1, MEMVIZ_TF_S1);
ASSERT_TRAPFRAME_SLOT(a0, MEMVIZ_TF_A0);
ASSERT_TRAPFRAME_SLOT(a1, MEMVIZ_TF_A1);
ASSERT_TRAPFRAME_SLOT(a2, MEMVIZ_TF_A2);
ASSERT_TRAPFRAME_SLOT(a3, MEMVIZ_TF_A3);
ASSERT_TRAPFRAME_SLOT(a4, MEMVIZ_TF_A4);
ASSERT_TRAPFRAME_SLOT(a5, MEMVIZ_TF_A5);
ASSERT_TRAPFRAME_SLOT(a6, MEMVIZ_TF_A6);
ASSERT_TRAPFRAME_SLOT(a7, MEMVIZ_TF_A7);
ASSERT_TRAPFRAME_SLOT(s2, MEMVIZ_TF_S2);
ASSERT_TRAPFRAME_SLOT(s3, MEMVIZ_TF_S3);
ASSERT_TRAPFRAME_SLOT(s4, MEMVIZ_TF_S4);
ASSERT_TRAPFRAME_SLOT(s5, MEMVIZ_TF_S5);
ASSERT_TRAPFRAME_SLOT(s6, MEMVIZ_TF_S6);
ASSERT_TRAPFRAME_SLOT(s7, MEMVIZ_TF_S7);
ASSERT_TRAPFRAME_SLOT(s8, MEMVIZ_TF_S8);
ASSERT_TRAPFRAME_SLOT(s9, MEMVIZ_TF_S9);
ASSERT_TRAPFRAME_SLOT(s10, MEMVIZ_TF_S10);
ASSERT_TRAPFRAME_SLOT(s11, MEMVIZ_TF_S11);
ASSERT_TRAPFRAME_SLOT(t3, MEMVIZ_TF_T3);
ASSERT_TRAPFRAME_SLOT(t4, MEMVIZ_TF_T4);
ASSERT_TRAPFRAME_SLOT(t5, MEMVIZ_TF_T5);
ASSERT_TRAPFRAME_SLOT(t6, MEMVIZ_TF_T6);

#undef ASSERT_TRAPFRAME_SLOT

static struct spinlock memvizsys_lock;
static int memvizsys_lock_ready;
static struct memviz_snapshot memvizsys_snapshot;

/**
 * ensure_memvizsys_lock 初始化 memsnapshot 系统调用的静态缓冲锁。
 *
 * 该锁只保护 sys_memsnapshot 内部复用的大型快照缓冲，避免把扩展后的
 * memviz_snapshot 放在每个进程只有一页的内核栈上。首次调用发生在普通
 * 进程上下文，重复 initlock 写入同一初值不会改变后续锁语义。
 */
static void
ensure_memvizsys_lock(void)
{
  if(!memvizsys_lock_ready){
    initlock(&memvizsys_lock, "memvizsys");
    memvizsys_lock_ready = 1;
  }
}

/**
 * fill_user_leaf_mapping 读取用户页表中一个固定页的叶子映射。
 *
 * @param pagetable 当前进程用户页表，只读访问。
 * @param va 要查询的页对齐虚拟地址。
 * @param pa 接收页对齐物理地址；未映射时写 0。
 * @param flags 接收 PTE 低十位 flags；未映射时写 0。
 */
static void
fill_user_leaf_mapping(pagetable_t pagetable, uint64 va,
                       uint64 *pa, uint64 *flags)
{
  *pa = 0;
  *flags = 0;

  pte_t *pte = walk(pagetable, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0)
    return;
  if((*pte & (PTE_R | PTE_W | PTE_X)) == 0)
    return;

  *pa = PTE2PA(*pte);
  *flags = PTE_FLAGS(*pte);
}

/**
 * fill_user_top_snapshot 补充用户页表顶端两个 supervisor-only 固定页。
 *
 * @param p 当前进程；调用期间页表、trapframe 与 trampoline 映射保持有效。
 * @param snapshot 已由 memviz_snapshot 填写的快照，函数在原地补充顶端布局。
 *
 * TRAPFRAME 的 36 个连续 uint64 槽按真实 ABI 原样复制，使用户态 renderer
 * 能同时展示页内偏移、虚拟地址、物理地址和采样值。TRAMPOLINE 的代码范围
 * 由汇编标签计算，不依赖手写指令长度。
 */
static void
fill_user_top_snapshot(struct proc *p, struct memviz_snapshot *snapshot)
{
  snapshot->maxva = MAXVA;
  snapshot->trapframe = TRAPFRAME;
  snapshot->trampoline = TRAMPOLINE;
  snapshot->trapframe_used = sizeof(struct trapframe);
  snapshot->uservec_offset = (uint64)uservec - (uint64)trampoline;
  snapshot->userret_offset = (uint64)userret - (uint64)trampoline;
  snapshot->trampoline_used = (uint64)trampoline_end - (uint64)trampoline;

  memmove(snapshot->trapframe_values, p->trapframe,
          sizeof(snapshot->trapframe_values));

  fill_user_leaf_mapping(p->pagetable, TRAPFRAME,
                         &snapshot->trapframe_pa,
                         &snapshot->trapframe_flags);
  fill_user_leaf_mapping(p->pagetable, TRAMPOLINE,
                         &snapshot->trampoline_pa,
                         &snapshot->trampoline_flags);
}

/**
 * sys_memsnapshot 将当前进程可观察到的内存状态复制到用户空间。
 *
 * 系统调用参数 0 为 MEMVIZ_VIEW_*，参数 1 为用户态输出结构体地址。
 * 采样函数返回前已经释放 allocator 锁；函数持有的 memvizsys_lock 只保护
 * 静态快照缓冲，防止多个进程并发 memsnapshot 时互相覆盖结果。
 *
 * @return 成功返回 0；参数、view 或 copyout 无效时返回 -1。
 */
uint64
sys_memsnapshot(void)
{
  int view;
  uint64 address;

  if(argint(0, &view) < 0 || argaddr(1, &address) < 0)
    return -1;

  ensure_memvizsys_lock();
  acquire(&memvizsys_lock);

  if(memviz_snapshot(view, &memvizsys_snapshot) < 0){
    release(&memvizsys_lock);
    return -1;
  }

  struct proc *p = myproc();
  fill_user_top_snapshot(p, &memvizsys_snapshot);
  if(copyout(p->pagetable, address, (char *)&memvizsys_snapshot,
             sizeof(memvizsys_snapshot)) < 0){
    release(&memvizsys_lock);
    return -1;
  }

  release(&memvizsys_lock);
  return 0;
}

/**
 * sys_vaquery 将当前进程中一个用户 VA 的页表链路复制到用户空间。
 *
 * 参数 0 是目标虚拟地址，参数 1 是 struct memviz_va_query 的用户地址。
 * 该系统调用只读页表元数据，不读取目标 VA 的内容，也不为目标 VA 分配页。
 *
 * @return 成功返回 0；参数、地址范围或 copyout 失败时返回 -1。
 */
uint64
sys_vaquery(void)
{
  uint64 va;
  uint64 address;
  struct memviz_va_query query;

  if(argaddr(0, &va) < 0 || argaddr(1, &address) < 0)
    return -1;
  if(memviz_query_user_va(va, &query) < 0)
    return -1;

  struct proc *p = myproc();
  if(copyout(p->pagetable, address, (char *)&query, sizeof(query)) < 0)
    return -1;
  return 0;
}
