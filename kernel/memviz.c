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
  snapshot->user_limit = USERMAX;
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
  snapshot->user_mirror_start = KUSERBASE;
  snapshot->user_mirror_end = KUSERADDR(p->sz);
}

/**
 * append_pte_entry 记录一个代表性虚拟地址对应的叶子 PTE。
 *
 * @param snapshot 待填写的快照；函数会追加一条固定大小记录。
 * @param space MEMVIZ_PTE_SPACE_*，表示查询用户页表还是内核页表。
 * @param role MEMVIZ_PTE_ROLE_*，表示该 VA 在渲染层中的教学标签。
 * @param pagetable 被查询的页表，不会被修改。
 * @param va 需要观察的虚拟地址，函数内部按页向下对齐。
 */
static void
append_pte_entry(struct memviz_snapshot *snapshot, int space, int role,
                 pagetable_t pagetable, uint64 va)
{
  if(snapshot->pagetable_entry_count >= MEMVIZ_PTE_ENTRIES)
    return;

  struct memviz_pte_entry *entry =
    &snapshot->pagetable_entries[snapshot->pagetable_entry_count++];
  entry->space = space;
  entry->role = role;
  entry->va = PGROUNDDOWN(va);

  for(int level = 2; level >= 0; level--){
    struct memviz_pte_level *pte_level = &entry->levels[2 - level];
    pte_level->level = level;
    pte_level->index = PX(level, entry->va);

    pte_t pte = pagetable[pte_level->index];
    pte_level->pte = pte;
    pte_level->flags = PTE_FLAGS(pte);
    if((pte & PTE_V) == 0)
      return;

    pte_level->present = 1;
    pte_level->pa = PTE2PA(pte);
    if(level == 0 || (pte & (PTE_R | PTE_W | PTE_X)) != 0){
      if((pte & (PTE_R | PTE_W | PTE_X)) == 0)
        return;
      entry->present = 1;
      entry->pte = pte;
      entry->pa = pte_level->pa;
      entry->flags = pte_level->flags;
      return;
    }

    pagetable = (pagetable_t)pte_level->pa;
  }
}

/**
 * fill_pte_path 只读记录一个 VA 在指定页表中的 Sv39 三级链路。
 *
 * @param pagetable 被查询页表，不会被修改。
 * @param va 用户传入的目标虚拟地址；函数内部按页向下对齐。
 * @param levels 接收 L2、L1、L0 三层 PTE 信息，必须有三个元素。
 * @param pte_out 接收 leaf PTE；没有有效 leaf 时写入 0。
 * @param flags_out 接收 leaf flags；没有有效 leaf 时写入 0。
 * @param pa_out 接收 leaf PA；没有有效 leaf 时写入 0。
 * @return 找到有效 leaf 映射时返回 1；中途缺页或没有 leaf 时返回 0。
 */
static int
fill_pte_path(pagetable_t pagetable, uint64 va,
              struct memviz_pte_level levels[3], uint64 *pte_out,
              uint64 *flags_out, uint64 *pa_out)
{
  uint64 aligned = PGROUNDDOWN(va);

  *pte_out = 0;
  *flags_out = 0;
  *pa_out = 0;

  if(aligned >= MAXVA)
    return 0;

  for(int level = 2; level >= 0; level--){
    struct memviz_pte_level *pte_level = &levels[2 - level];
    pte_level->level = level;
    pte_level->index = PX(level, aligned);

    pte_t pte = pagetable[pte_level->index];
    pte_level->pte = pte;
    pte_level->flags = PTE_FLAGS(pte);
    if((pte & PTE_V) == 0)
      return 0;

    pte_level->present = 1;
    pte_level->pa = PTE2PA(pte);
    if(level == 0 || (pte & (PTE_R | PTE_W | PTE_X)) != 0){
      if((pte & (PTE_R | PTE_W | PTE_X)) == 0)
        return 0;
      *pte_out = pte;
      *flags_out = pte_level->flags;
      *pa_out = pte_level->pa;
      return 1;
    }

    pagetable = (pagetable_t)pte_level->pa;
  }

  return 0;
}

/**
 * fill_pagetable_observations 采集从 VA 到 PA 的代表性映射闭环。
 *
 * @param p 当前进程；调用期间页表稳定，函数只读 walk() 查询结果。
 * @param snapshot 待填写的快照；调用者已经清零并填好布局边界。
 *
 * 本视图不递归打印完整页表树，避免在普通 memviz 命令中输出成百上千行。
 * 采样点覆盖用户 ELF、guard、栈、dynamic、内核 user alias、内核栈、
 * trampoline、direct map 与 MMIO，足够解释“用户 VA 如何经页表落到 PA，
 * 再通过别名窗口被内核访问”。
 */
static void
fill_pagetable_observations(struct proc *p, struct memviz_snapshot *snapshot)
{
  snapshot->user_pagetable = (uint64)p->pagetable;

  append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_USER,
                   MEMVIZ_PTE_ROLE_ELF_FIRST, p->pagetable, 0);
  if(snapshot->image_end > PGSIZE)
    append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_USER,
                     MEMVIZ_PTE_ROLE_ELF_LAST, p->pagetable,
                     snapshot->image_end - PGSIZE);
  if(snapshot->user_stack_valid){
    append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_USER,
                     MEMVIZ_PTE_ROLE_GUARD, p->pagetable,
                     snapshot->stack_guard_start);
    append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_USER,
                     MEMVIZ_PTE_ROLE_USER_STACK, p->pagetable,
                     snapshot->stack_bottom);
  }
  if(snapshot->process_size > snapshot->dynamic_start)
    append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_USER,
                     MEMVIZ_PTE_ROLE_DYNAMIC, p->pagetable,
                     snapshot->dynamic_start);

  append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_KERNEL,
                   MEMVIZ_PTE_ROLE_USER_MIRROR, p->kpagetable, KUSERBASE);
  append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_KERNEL,
                   MEMVIZ_PTE_ROLE_KERNEL_STACK_GUARD, p->kpagetable,
                   snapshot->kernel_stack_guard_start);
  append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_KERNEL,
                   MEMVIZ_PTE_ROLE_KERNEL_STACK, p->kpagetable,
                   snapshot->kernel_stack_bottom);
  append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_KERNEL,
                   MEMVIZ_PTE_ROLE_TRAMPOLINE, p->kpagetable,
                   snapshot->trampoline);
  append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_KERNEL,
                   MEMVIZ_PTE_ROLE_KERNEL_TEXT, p->kpagetable,
                   snapshot->kernel_text_start);
  append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_KERNEL,
                   MEMVIZ_PTE_ROLE_RAM_DIRECT, p->kpagetable,
                   snapshot->kernel_data_start);
  append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_KERNEL,
                   MEMVIZ_PTE_ROLE_UART, p->kpagetable, snapshot->uart_start);
  append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_KERNEL,
                   MEMVIZ_PTE_ROLE_VIRTIO, p->kpagetable,
                   snapshot->virtio_start);
  append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_KERNEL,
                   MEMVIZ_PTE_ROLE_CLINT, p->kpagetable, snapshot->clint_start);
  append_pte_entry(snapshot, MEMVIZ_PTE_SPACE_KERNEL,
                   MEMVIZ_PTE_ROLE_PLIC, p->kpagetable, snapshot->plic_start);
}

/**
 * append_pt_usage_page 记录一个已分配页表页的 PTE 槽占用矩阵。
 *
 * @param snapshot 待填写的快照；函数会追加一个页表页摘要。
 * @param space MEMVIZ_PTE_SPACE_*，表示属于用户页表还是内核页表。
 * @param level Sv39 页表层级，2 为根页表，0 为 leaf PTE 所在页表。
 * @param pagetable 当前页表页的内核可访问地址。
 *
 * 每个 cell 覆盖连续 PTE 槽，只记录 valid PTE 数量，不追踪普通数据页内容。
 */
static void
append_pt_usage_page(struct memviz_snapshot *snapshot, int space, int level,
                     pagetable_t pagetable)
{
  if(snapshot->pagetable_usage_count >= MEMVIZ_PT_USAGE_PAGES)
    return;

  struct memviz_pt_usage_page *page =
    &snapshot->pagetable_usage[snapshot->pagetable_usage_count++];
  page->space = space;
  page->level = level;
  page->pa = (uint64)pagetable;
  page->total_entries = 512;

  for(int i = 0; i < 512; i++){
    int cell = (i * MEMVIZ_PT_USAGE_CELLS) / 512;
    page->cells[cell].total_entries++;
    if((pagetable[i] & PTE_V) != 0){
      page->cells[cell].used_entries++;
      page->used_entries++;
    }
  }
}

/**
 * scan_pt_usage 递归扫描已分配页表页的占用情况。
 *
 * @param snapshot 待填写的快照。
 * @param space MEMVIZ_PTE_SPACE_*，表示所属地址空间。
 * @param level 当前 Sv39 层级。
 * @param pagetable 当前页表页的内核可访问地址。
 *
 * 只沿 valid 且非 leaf 的 PTE 继续递归，因此不会遍历普通物理数据页。
 */
static void
scan_pt_usage(struct memviz_snapshot *snapshot, int space, int level,
              pagetable_t pagetable)
{
  append_pt_usage_page(snapshot, space, level, pagetable);
  if(level == 0 || snapshot->pagetable_usage_count >= MEMVIZ_PT_USAGE_PAGES)
    return;

  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) == 0)
      continue;
    if((pte & (PTE_R | PTE_W | PTE_X)) != 0)
      continue;
    scan_pt_usage(snapshot, space, level - 1, (pagetable_t)PTE2PA(pte));
    if(snapshot->pagetable_usage_count >= MEMVIZ_PT_USAGE_PAGES)
      return;
  }
}

/**
 * fill_pagetable_usage 采集用户页表和内核页表页的槽位余量。
 *
 * @param p 当前进程；函数只读 p->pagetable 和 p->kpagetable。
 * @param snapshot 待填写的快照。
 */
static void
fill_pagetable_usage(struct proc *p, struct memviz_snapshot *snapshot)
{
  scan_pt_usage(snapshot, MEMVIZ_PTE_SPACE_USER, 2, p->pagetable);
  scan_pt_usage(snapshot, MEMVIZ_PTE_SPACE_KERNEL, 2, p->kpagetable);
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
     view != MEMVIZ_VIEW_KERNEL && view != MEMVIZ_VIEW_PAGETABLE)
    return -1;

  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->view = view;

  struct proc *p = myproc();
  fill_user_layout(p, snapshot);
  fill_kernel_layout(p, snapshot);
  fill_pagetable_observations(p, snapshot);
  fill_pagetable_usage(p, snapshot);

  // kalloc 负责在锁内完成 freelist 采样，返回前已经释放所有 allocator 锁。
  kalloc_mem_snapshot(snapshot);
  return 0;
}

/**
 * memviz_query_user_va 查询当前进程用户页表中一个 VA 的页表链路。
 *
 * @param va 目标用户虚拟地址；允许未映射，函数会返回缺失层级信息。
 * @param query 输出查询结果，必须是可写内核地址。
 * @return 参数有效时返回 0；地址超出普通用户 VA 范围或 query 为空时返回 -1。
 *
 * 该接口只观察 PTE，不读取或写入 va 指向的数据页；因此不会替代用户态
 * load/store fault 实验，也不会触发 lazy allocation 或 COW。
 */
int
memviz_query_user_va(uint64 va, struct memviz_va_query *query)
{
  if(query == 0 || va >= USERMAX)
    return -1;

  memset(query, 0, sizeof(*query));
  query->va = PGROUNDDOWN(va);

  struct proc *p = myproc();
  query->present = fill_pte_path(p->pagetable, query->va, query->levels,
                                 &query->pte, &query->flags, &query->pa);

  uint64 kalloc_start = PGROUNDUP((uint64)end);
  uint64 total = (PHYSTOP - kalloc_start) / PGSIZE;
  query->kalloc_cell = -1;
  if(query->present && total > 0 && query->pa >= kalloc_start &&
     query->pa < PHYSTOP){
    uint64 page = (query->pa - kalloc_start) / PGSIZE;
    query->kalloc_cell = (page * MEMVIZ_CELLS) / total;
  }

  return 0;
}
