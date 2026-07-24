#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "vma.h"
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
 * dynamic_cell_bounds 返回一个压缩单元覆盖的页下标半开区间。
 *
 * @param snapshot 已初始化动态页总数和单元数量的快照。
 * @param cell 单元下标，范围为 [0, dynamic_state_cell_count)。
 * @param first 接收首个页下标。
 * @param end 接收末尾后一页下标。
 */
static void
dynamic_cell_bounds(struct memviz_snapshot *snapshot, int cell,
                    uint64 *first, uint64 *end)
{
  uint64 cells = snapshot->dynamic_state_cell_count;
  uint64 pages = snapshot->dynamic_page_count;
  *first = (uint64)cell * pages / cells;
  *end = (uint64)(cell + 1) * pages / cells;
}

/**
 * interval_overlap_pages 计算两个页下标半开区间的交集页数。
 *
 * @param first_a 第一个区间起点。
 * @param end_a 第一个区间终点。
 * @param first_b 第二个区间起点。
 * @param end_b 第二个区间终点。
 * @return 两区间重叠的页数。
 */
static uint64
interval_overlap_pages(uint64 first_a, uint64 end_a,
                       uint64 first_b, uint64 end_b)
{
  uint64 first = first_a > first_b ? first_a : first_b;
  uint64 end = end_a < end_b ? end_a : end_b;
  return end > first ? end - first : 0;
}

/**
 * initialize_dynamic_cells 建立动态范围的固定宽度压缩桶，初始均视为 lazy。
 *
 * @param snapshot 已包含 dynamic_start 和 process_size 的快照。
 *
 * 当动态范围不超过 32 页时，每个单元恰好覆盖一页；更大范围按连续地址
 * 等比例压缩。初始 lazy 只是“尚未观察到 VMA 或有效叶子”的默认状态，
 * 后续步骤会按 VMA 和 PTE 事实覆盖它。
 */
static void
initialize_dynamic_cells(struct memviz_snapshot *snapshot)
{
  if(snapshot->process_size <= snapshot->dynamic_start)
    return;

  uint64 end = PGROUNDUP(snapshot->process_size);
  snapshot->dynamic_page_count =
    (end - snapshot->dynamic_start) / PGSIZE;
  snapshot->dynamic_state_cell_count = snapshot->dynamic_page_count;
  if(snapshot->dynamic_state_cell_count > MEMVIZ_USER_STATE_CELLS)
    snapshot->dynamic_state_cell_count = MEMVIZ_USER_STATE_CELLS;

  for(int cell = 0; cell < (int)snapshot->dynamic_state_cell_count; cell++){
    uint64 first;
    uint64 cell_end;
    dynamic_cell_bounds(snapshot, cell, &first, &cell_end);
    struct memviz_user_state_cell *state = &snapshot->dynamic_state[cell];
    state->total_pages = cell_end - first;
    state->lazy_pages = state->total_pages;
  }
}

/**
 * overlay_vma_ranges 将活动 VMA 覆盖的逻辑页从 lazy 改记为 mmap。
 *
 * @param p 当前进程；函数只读 p->vma[] 和 VMA 边界。
 * @param snapshot 已初始化动态压缩单元的快照。
 *
 * 这里只根据 VMA 元数据标记整段逻辑区域，不读取文件、不建立 PTE。后续扫描
 * 有效叶子时，带 PTE_COW 的 mmap 页会再次提升为 COW，保持最终优先级。
 */
static void
overlay_vma_ranges(struct proc *p, struct memviz_snapshot *snapshot)
{
  if(snapshot->dynamic_page_count == 0)
    return;

  uint64 range_start = snapshot->dynamic_start;
  uint64 range_end = PGROUNDUP(snapshot->process_size);

  for(int index = 0; index < NOFILE; index++){
    struct VMA *v = p->vma[index];
    if(v == 0 || !v->used || v->length == 0)
      continue;

    uint64 vma_end_raw = v->addr + v->length;
    if(vma_end_raw < v->addr)
      continue;
    uint64 vma_start = PGROUNDDOWN(v->addr);
    uint64 vma_end = PGROUNDUP(vma_end_raw);
    if(vma_start < range_start)
      vma_start = range_start;
    if(vma_end > range_end)
      vma_end = range_end;
    if(vma_end <= vma_start)
      continue;

    uint64 first_page = (vma_start - range_start) / PGSIZE;
    uint64 end_page = (vma_end - range_start) / PGSIZE;
    for(int cell = 0; cell < (int)snapshot->dynamic_state_cell_count; cell++){
      uint64 cell_first;
      uint64 cell_end;
      dynamic_cell_bounds(snapshot, cell, &cell_first, &cell_end);
      uint64 overlap = interval_overlap_pages(first_page, end_page,
                                              cell_first, cell_end);
      if(overlap == 0)
        continue;

      struct memviz_user_state_cell *state = &snapshot->dynamic_state[cell];
      if(overlap > state->lazy_pages)
        overlap = state->lazy_pages;
      state->lazy_pages -= overlap;
      state->mmap_pages += overlap;
    }
  }
}

/**
 * classify_dynamic_leaf 将一个有效 L0 用户叶子覆盖到对应动态压缩单元。
 *
 * @param p 当前进程，用于判断该 VA 是否仍属于活动 VMA。
 * @param snapshot 已完成默认 lazy 和 VMA 覆盖的快照。
 * @param va 页对齐用户虚拟地址。
 * @param pte 该地址的有效叶子 PTE。
 *
 * 最终优先级为 COW > mmap > resident > lazy。普通 mmap 驻留页仍显示为
 * mmap，以表达区域来源；只有其 PTE 被 fork 转换为 COW 时才提升为 COW。
 */
static void
classify_dynamic_leaf(struct proc *p, struct memviz_snapshot *snapshot,
                      uint64 va, pte_t pte)
{
  if(va < snapshot->dynamic_start || va >= PGROUNDUP(snapshot->process_size))
    return;
  if((pte & PTE_V) == 0 || (pte & PTE_U) == 0)
    return;

  uint64 page = (va - snapshot->dynamic_start) / PGSIZE;
  uint64 cell = page * snapshot->dynamic_state_cell_count /
                snapshot->dynamic_page_count;
  if(cell >= snapshot->dynamic_state_cell_count)
    return;

  struct memviz_user_state_cell *state = &snapshot->dynamic_state[cell];
  int is_mmap = vma_find(p, va) != 0;
  if(pte & PTE_COW){
    if(is_mmap){
      if(state->mmap_pages > 0)
        state->mmap_pages--;
    } else if(state->lazy_pages > 0){
      state->lazy_pages--;
    }
    state->cow_pages++;
    return;
  }

  if(is_mmap)
    return;
  if(state->lazy_pages > 0)
    state->lazy_pages--;
  state->resident_pages++;
}

/**
 * scan_dynamic_leaves 只遍历与动态范围相交的有效页表子树。
 *
 * @param p 当前进程。
 * @param snapshot 动态页状态快照。
 * @param pagetable 当前页表页。
 * @param level 当前 Sv39 层级，根为 2、叶为 0。
 * @param base 当前页表页下标 0 对应的虚拟地址。
 *
 * xv6 用户映射由 mappages() 建立在 L0。本观察器遇到高层 leaf 时跳过，避免
 * 为未来可能出现的大页映射逐页展开巨量范围；当前实现不会产生这种用户大页。
 */
static void
scan_dynamic_leaves(struct proc *p, struct memviz_snapshot *snapshot,
                    pagetable_t pagetable, int level, uint64 base)
{
  uint64 span = PGSIZE;
  for(int current = 0; current < level; current++)
    span *= 512;

  uint64 range_start = snapshot->dynamic_start;
  uint64 range_end = PGROUNDUP(snapshot->process_size);
  for(int index = 0; index < 512; index++){
    uint64 va = base + (uint64)index * span;
    if(va >= range_end)
      break;
    if(va + span <= range_start)
      continue;

    pte_t pte = pagetable[index];
    if((pte & PTE_V) == 0)
      continue;
    if(pte & (PTE_R | PTE_W | PTE_X)){
      if(level == 0)
        classify_dynamic_leaf(p, snapshot, va, pte);
      continue;
    }
    if(level > 0)
      scan_dynamic_leaves(p, snapshot, (pagetable_t)PTE2PA(pte),
                          level - 1, va);
  }
}

/**
 * finish_dynamic_totals 汇总压缩单元并验证每个单元仍覆盖相同页数。
 *
 * @param snapshot 已完成 VMA 与 PTE 覆盖的快照。
 */
static void
finish_dynamic_totals(struct memviz_snapshot *snapshot)
{
  for(int cell = 0; cell < (int)snapshot->dynamic_state_cell_count; cell++){
    struct memviz_user_state_cell *state = &snapshot->dynamic_state[cell];
    uint64 classified = state->resident_pages + state->cow_pages +
                        state->lazy_pages + state->mmap_pages;
    if(classified != state->total_pages)
      panic("memviz user state");
    snapshot->dynamic_resident_pages += state->resident_pages;
    snapshot->dynamic_cow_pages += state->cow_pages;
    snapshot->dynamic_lazy_pages += state->lazy_pages;
    snapshot->dynamic_mmap_pages += state->mmap_pages;
  }
}

/**
 * fill_user_page_states 只读采集 dynamic_start 到 p->sz 的页级状态。
 *
 * @param p 当前进程；系统调用期间不会替换其页表或 VMA 数组。
 * @param snapshot 已由 memviz_snapshot 清零并填写基本布局的快照。
 *
 * 本函数不调用 walkaddr()、cow_alloc() 或 mmap_fault()，因此不会分配物理页、
 * 修改 PTE、复制 COW 页或读取 mmap 文件内容。
 */
static void
fill_user_page_states(struct proc *p, struct memviz_snapshot *snapshot)
{
  initialize_dynamic_cells(snapshot);
  if(snapshot->dynamic_page_count == 0)
    return;
  overlay_vma_ranges(p, snapshot);
  scan_dynamic_leaves(p, snapshot, p->pagetable, 2, 0);
  finish_dynamic_totals(snapshot);
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
  fill_user_page_states(p, &memvizsys_snapshot);
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
