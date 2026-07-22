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
} kmem[NCPU];

#define NPHYPAGES ((PHYSTOP - KERNBASE) / PGSIZE)

struct {
  struct spinlock lock;
  int count[NPHYPAGES];
} pageref;

static int
pa_index(uint64 pa)
{
  if(pa < KERNBASE || pa >= PHYSTOP || (pa % PGSIZE) != 0)
    panic("pa_index");
  return (pa - KERNBASE) / PGSIZE;
}

static int
page_refcount(uint64 pa)
{
  int refs;

  acquire(&pageref.lock);
  refs = pageref.count[pa_index(pa)];
  release(&pageref.lock);

  return refs;
}

static void
cow_install_writable_page(pagetable_t pagetable, uint64 va,
                          pte_t *pte, uint64 pa, uint flags)
{
  *pte = PA2PTE(pa) | flags;

  struct proc *p = myproc();
  if(p && p->pagetable == pagetable)
    u2kvmcopy(p->pagetable, p->kpagetable, va, va + PGSIZE);

  sfence_vma();
}

void
kinit()
{
  for(int i = 0; i < NCPU; i++)
    initlock(&kmem[i].lock, "kmem");
  initlock(&pageref.lock, "pageref");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    acquire(&pageref.lock);
    pageref.count[pa_index((uint64)p)] = 1;
    release(&pageref.lock);
    kfree(p);
  }
}

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

  memset(pa, 1, PGSIZE);
  r = (struct run*)pa;

  push_off();
  int id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
  pop_off();
}

void *
kalloc(void)
{
  struct run *r = 0;

  push_off();
  int id = cpuid();

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  if(!r){
    for(int i = 0; i < NCPU; i++){
      if(i == id)
        continue;
      acquire(&kmem[i].lock);
      r = kmem[i].freelist;
      if(r)
        kmem[i].freelist = r->next;
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
    memset((char*)r, 5, PGSIZE);
  }
  return (void*)r;
}

uint64
free_mem(void)
{
  uint64 pages = 0;

  for(int i = 0; i < NCPU; i++){
    acquire(&kmem[i].lock);
    for(struct run *r = kmem[i].freelist; r; r = r->next)
      pages++;
    release(&kmem[i].lock);
  }
  return pages * PGSIZE;
}

/**
 * kalloc_mem_snapshot 按物理地址采集 allocator 管理页的空闲分布。
 *
 * @param snapshot 输出快照；调用者必须先清零，函数会填写 kalloc 相关字段。
 *
 * 所有 kmem 锁按 CPU 编号递增获取、递减释放。持锁期间只遍历 freelist 和
 * 写内核栈上的快照，不打印、不 copyout，也不申请新页。
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

  for(int i = 0; i < NCPU; i++)
    acquire(&kmem[i].lock);

  uint64 free = 0;
  for(int cpu = 0; cpu < NCPU; cpu++){
    for(struct run *r = kmem[cpu].freelist; r; r = r->next){
      uint64 pa = (uint64)r;
      if(pa < start || pa >= PHYSTOP || ((pa - start) % PGSIZE) != 0)
        panic("kalloc snapshot");

      uint64 page = (pa - start) / PGSIZE;
      int cell = (page * MEMVIZ_CELLS) / total;
      snapshot->physical[cell].free_pages++;
      snapshot->cpu_free_pages[cpu]++;
      free++;
    }
  }

  for(int i = NCPU - 1; i >= 0; i--)
    release(&kmem[i].lock);

  snapshot->free_pages = free;
  snapshot->used_pages = total - free;
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
