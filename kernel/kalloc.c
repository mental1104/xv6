// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

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
  va = PGROUNDDOWN(va);
  if(va >= MAXVA)
    return -1;

  pte_t *pte = walk(pagetable, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_COW) == 0)
    return -1;

  uint64 pa = PTE2PA(*pte);
  int refs;
  acquire(&pageref.lock);
  refs = pageref.count[pa_index(pa)];
  release(&pageref.lock);

  if(refs == 1){
    *pte = (*pte | PTE_W) & ~PTE_COW;
    sfence_vma();
    return 0;
  }

  char *mem = kalloc();
  if(mem == 0)
    return -1;

  memmove(mem, (void*)pa, PGSIZE);
  uint flags = (PTE_FLAGS(*pte) | PTE_W) & ~PTE_COW;
  *pte = PA2PTE((uint64)mem) | flags;
  sfence_vma();
  kfree((void*)pa);
  return 0;
}
