// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include "memviz.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  uint64 free_count;
} kmem[NCPU];

/**
 * kalloc 页面生命周期的调试填充值。
 *
 * 0xA5 与 0x5A 互为按位取反，十六进制转储中容易辨认，并避开 0、1
 * 等常见初始化值。它们只表示页面仍保持“刚分配”或“刚释放”的未覆写
 * 状态，不能证明调用者属于用户态或内核态，也不能作为所有权判定依据。
 */
enum kalloc_page_fill {
  KALLOC_PAGE_FILL_ALLOCATED = 0xA5,
  KALLOC_PAGE_FILL_FREED = 0x5A,
};

_Static_assert(KALLOC_PAGE_FILL_ALLOCATED != KALLOC_PAGE_FILL_FREED,
               "kalloc page fill patterns must differ");
_Static_assert(KALLOC_PAGE_FILL_ALLOCATED <= 0xFF &&
               KALLOC_PAGE_FILL_FREED <= 0xFF,
               "kalloc page fill patterns must fit one byte");

#define NPHYPAGES ((PHYSTOP - KERNBASE) / PGSIZE)
#define KALLOC_AUDIT_BYTES ((NPHYPAGES + 7) / 8)

struct {
  struct spinlock lock;
  int count[NPHYPAGES];
} pageref;

// 审计位图只在 memviz 快照期间复用；每一位表示对应物理页是否已在任一
// CPU freelist 中出现，用于在损坏链表上检测重复节点和环而不无限遍历。
struct {
  struct spinlock lock;
  uchar seen[KALLOC_AUDIT_BYTES];
} kalloc_audit;

/**
 * pa_index 将页对齐物理地址换算为 pageref 与审计位图的稳定下标。
 *
 * @param pa 位于 [KERNBASE, PHYSTOP) 的页对齐物理地址。
 * @return 对应物理页下标；参数越界或未对齐时触发 panic。
 */
static int
pa_index(uint64 pa)
{
  if(pa < KERNBASE || pa >= PHYSTOP || (pa % PGSIZE) != 0)
    panic("pa_index");
  return (pa - KERNBASE) / PGSIZE;
}

/**
 * page_refcount 读取一个已分配物理页的引用计数。
 *
 * @param pa 位于 pageref 管理范围内的页对齐物理地址。
 * @return 当前引用数；读取期间由 pageref.lock 保护。
 */
static int
page_refcount(uint64 pa)
{
  int refs;

  acquire(&pageref.lock);
  refs = pageref.count[pa_index(pa)];
  release(&pageref.lock);

  return refs;
}

/**
 * audit_mark_page 在本轮 allocator 快照中登记一个空闲页。
 *
 * @param pa 已完成范围和对齐校验的物理页地址。
 * @return 首次出现返回 1；同一页已被其他链表节点登记时返回 0。
 *
 * 调用者必须持有 kalloc_audit.lock。重复节点既可能来自跨 CPU 重复挂链，
 * 也可能来自单链表形成环；返回 0 后调用者应停止跟随该链，避免死循环。
 */
static int
audit_mark_page(uint64 pa)
{
  int index = pa_index(pa);
  int byte = index / 8;
  uchar mask = (uchar)(1U << (index % 8));

  if(kalloc_audit.seen[byte] & mask)
    return 0;
  kalloc_audit.seen[byte] |= mask;
  return 1;
}

/**
 * cow_install_writable_page 将当前 COW 叶子替换为可写映射并刷新内核别名。
 *
 * @param pagetable 待修改的用户页表。
 * @param va 页对齐用户虚拟地址。
 * @param pte 目标叶子 PTE。
 * @param pa 新映射的页对齐物理地址。
 * @param flags 新 PTE 权限位。
 */
static void
cow_install_writable_page(pagetable_t pagetable, uint64 va,
                          pte_t *pte, uint64 pa, uint flags)
{
  *pte = PA2PTE(pa) | flags;

  struct proc *p = myproc();
  if(p && p->pagetable == pagetable &&
     u2kvmcopy(p->pagetable, p->kpagetable, va, va + PGSIZE) < 0)
    // 每个当前用户叶子在进入 COW 前都必须已有 alias 路径；这里失败说明
    // 生命周期不变量被破坏，而不是一个可由当前写故障安全回滚的普通 OOM。
    panic("cow alias");

  sfence_vma();
}

/** 初始化每 CPU 空闲链表、页引用计数和只读审计工作区。 */
void
kinit()
{
  for(int i = 0; i < NCPU; i++){
    initlock(&kmem[i].lock, "kmem");
    kmem[i].freelist = 0;
    kmem[i].free_count = 0;
  }
  initlock(&pageref.lock, "pageref");
  initlock(&kalloc_audit.lock, "kalloc_audit");
  freerange(end, (void*)PHYSTOP);
}

/**
 * freerange 将启动时尚未使用的物理页一次性挂入 CPU 0 freelist。
 *
 * @param pa_start 可回收物理区间的起点；函数内部向上按页对齐。
 * @param pa_end 可回收物理区间的一过终点，不会加入该地址所在页面。
 *
 * 启动阶段只有 hart 0 执行 kinit，因此可以在各持一次锁的临界区内批量
 * 初始化引用计数、链表和独立计数器。这里故意不复用 kfree()：正常 kfree()
 * 会把整页写成毒化字节，用于发现释放后使用；若对 2 GiB 初始 RAM 的每一页
 * 都做 4 KiB memset，会让每次 QEMU 启动无意义地写满全部物理内存。kalloc()
 * 仍会在页面真正分配时写入调试字节，因此该优化不改变已分配页的初始化语义。
 */
void
freerange(void *pa_start, void *pa_end)
{
  char *p = (char*)PGROUNDUP((uint64)pa_start);

  acquire(&pageref.lock);
  acquire(&kmem[0].lock);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    struct run *r = (struct run*)p;
    pageref.count[pa_index((uint64)p)] = 0;
    r->next = kmem[0].freelist;
    kmem[0].freelist = r;
    kmem[0].free_count++;
  }
  release(&kmem[0].lock);
  release(&pageref.lock);
}

/**
 * kfree 释放一个物理页引用，并在最后一个引用消失时归还当前 CPU freelist。
 *
 * @param pa 页对齐物理地址；必须来自 kalloc 管理范围且当前引用数为正。
 *
 * 重复释放由 pageref 的 `ref <= 0` 守卫触发 `panic("kfree ref")`，这是
 * allocator 的负向 oracle。页面只有在引用数降到 0 后才会按
 * KALLOC_PAGE_FILL_FREED 毒化并重新挂链；freelist 指针和 free_count 始终在
 * 同一个 kmem 锁临界区内同步更新。
 */
void
kfree(void *pa)
{
  struct run *r;
  int should_free = 0;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&pageref.lock);
  int *ref = &pageref.count[pa_index((uint64)pa)];
  if(*ref <= 0)
    panic("kfree ref");
  (*ref)--;
  if(*ref == 0)
    should_free = 1;
  release(&pageref.lock);

  if(!should_free)
    return;

  memset(pa, KALLOC_PAGE_FILL_FREED, PGSIZE);
  r = (struct run*)pa;

  push_off();
  int id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  kmem[id].free_count++;
  release(&kmem[id].lock);
  pop_off();
}

/**
 * kalloc 从当前 CPU freelist 取一页；本地为空时依次从其他 CPU 偷取一页。
 *
 * @return 成功时返回一页物理内存，所有权引用计数设为 1；耗尽时返回 0。
 *
 * 该固定粒度分配器只执行链表头部移除，不实现可变大小分割、相邻合并或
 * first/best/worst-fit。freelist 与 free_count 在同一锁内更新；若链表非空但
 * 计数为 0，说明元数据已失配并立即 panic，而不是让计数静默下溢。成功页会按
 * KALLOC_PAGE_FILL_ALLOCATED 填充，供调试时识别尚未被调用者覆写的内容。
 */
void *
kalloc(void)
{
  struct run *r = 0;

  push_off();
  int id = cpuid();

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r){
    if(kmem[id].free_count == 0)
      panic("kalloc count");
    kmem[id].freelist = r->next;
    kmem[id].free_count--;
  }
  release(&kmem[id].lock);

  if(!r){
    for(int i = 0; i < NCPU; i++){
      if(i == id)
        continue;
      acquire(&kmem[i].lock);
      r = kmem[i].freelist;
      if(r){
        if(kmem[i].free_count == 0)
          panic("kalloc count");
        kmem[i].freelist = r->next;
        kmem[i].free_count--;
      }
      release(&kmem[i].lock);
      if(r)
        break;
    }
  }
  pop_off();

  if(r){
    acquire(&pageref.lock);
    pageref.count[pa_index((uint64)r)] = 1;
    release(&pageref.lock);
    memset((char*)r, KALLOC_PAGE_FILL_ALLOCATED, PGSIZE);
  }
  return (void*)r;
}

uint64
free_mem(void)
{
  uint64 pages = 0;

  for(int i = 0; i < NCPU; i++){
    acquire(&kmem[i].lock);
    pages += kmem[i].free_count;
    release(&kmem[i].lock);
  }
  return pages * PGSIZE;
}

/**
 * kalloc_mem_snapshot 按物理地址采集 allocator 管理页的空闲分布并审计元数据。
 *
 * @param snapshot 输出快照；调用者必须先清零，函数会填写 kalloc 相关字段。
 *
 * 所有 kmem 锁按 CPU 编号递增获取、递减释放。持锁期间只遍历 freelist 和
 * 写静态审计位图/内核快照，不打印、不 copyout，也不申请新页。独立 free_count
 * 与实际链表遍历数必须相等；审计位图同时阻止重复节点或环导致无限遍历。
 */
void
kalloc_mem_snapshot(struct memviz_snapshot *snapshot)
{
  uint64 start = PGROUNDUP((uint64)end);
  uint64 total = (PHYSTOP - start) / PGSIZE;

  snapshot->kalloc_start = start;
  snapshot->kalloc_end = PHYSTOP;
  snapshot->total_pages = total;

  // total_pages 与后续 free page 使用同一映射公式，避免边界取整不一致。
  for(uint64 page = 0; page < total; page++){
    int cell = (page * MEMVIZ_CELLS) / total;
    snapshot->physical[cell].total_pages++;
  }

  acquire(&kalloc_audit.lock);
  memset(kalloc_audit.seen, 0, sizeof(kalloc_audit.seen));
  for(int i = 0; i < NCPU; i++)
    acquire(&kmem[i].lock);

  uint64 listed_free = 0;
  uint64 counter_free = 0;
  for(int cpu = 0; cpu < NCPU; cpu++){
    uint64 cpu_listed = 0;
    counter_free += kmem[cpu].free_count;

    for(struct run *r = kmem[cpu].freelist; r; r = r->next){
      uint64 pa = (uint64)r;
      if(pa < start || pa >= PHYSTOP || ((pa - start) % PGSIZE) != 0){
        snapshot->allocator_invalid_nodes++;
        break;
      }
      if(!audit_mark_page(pa)){
        snapshot->allocator_duplicate_pages++;
        break;
      }

      uint64 page = (pa - start) / PGSIZE;
      int cell = (page * MEMVIZ_CELLS) / total;
      snapshot->physical[cell].free_pages++;
      snapshot->cpu_free_pages[cpu]++;
      cpu_listed++;
      listed_free++;
    }

    if(cpu_listed != kmem[cpu].free_count)
      snapshot->allocator_count_mismatches++;
  }

  for(int i = NCPU - 1; i >= 0; i--)
    release(&kmem[i].lock);
  release(&kalloc_audit.lock);

  snapshot->free_pages = listed_free;
  snapshot->used_pages = listed_free <= total ? total - listed_free : 0;
  snapshot->allocator_counter_free_pages = counter_free;
  snapshot->allocator_invariant_ok =
    listed_free <= total && listed_free == counter_free &&
    snapshot->allocator_duplicate_pages == 0 &&
    snapshot->allocator_invalid_nodes == 0 &&
    snapshot->allocator_count_mismatches == 0;
}

void
increase_rc(uint64 pa)
{
  acquire(&pageref.lock);
  int *ref = &pageref.count[pa_index(pa)];
  if(*ref <= 0)
    panic("increase_rc");
  (*ref)++;
  release(&pageref.lock);
}

int
cow_alloc(pagetable_t pagetable, uint64 va)
{
  // Locate and validate the COW leaf PTE.
  va = PGROUNDDOWN(va);
  if(va >= MAXVA)
    return -1;

  pte_t *pte = walk(pagetable, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_COW) == 0)
    return -1;

  uint64 oldpa = PTE2PA(*pte);
  uint flags = (PTE_FLAGS(*pte) | PTE_W) & ~PTE_COW;

  // An exclusively owned page only needs its write permission restored.
  if(page_refcount(oldpa) == 1){
    cow_install_writable_page(pagetable, va, pte, oldpa, flags);
    return 0;
  }

  // A shared page must be copied before the current page table can write it.
  char *mem = kalloc();
  if(mem == 0)
    return -1;
  memmove(mem, (void*)oldpa, PGSIZE);

  // Commit the new mapping before dropping this page table's old reference.
  cow_install_writable_page(pagetable, va, pte, (uint64)mem, flags);
  kfree((void*)oldpa);
  return 0;
}
