#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "proc.h"
#include "fcntl.h"
#include "vma.h"

struct {
  struct spinlock lock;
  struct VMA areas[NPROC * NOFILE];
} vma_table;

void
vma_init(void)
{
  initlock(&vma_table.lock, "vma_table");
}

struct VMA*
vma_alloc(void)
{
  acquire(&vma_table.lock);
  // TODO: 这里的vma_table.areas是每个进程×NOFILE的大小，那么理论上一个进程可以把所有VMA份额全部用掉，是合理的吗？
  for(struct VMA *v = vma_table.areas;
      v < vma_table.areas + NPROC * NOFILE; v++){
    if(!v->used){
      memset(v, 0, sizeof(*v));
      v->used = 1;
      release(&vma_table.lock);
      return v;
    }
  }
  release(&vma_table.lock);
  return 0;
}

void
vma_free(struct VMA *v)
{
  acquire(&vma_table.lock);
  memset(v, 0, sizeof(*v)); // 这里会隐式地把 v->used 置为 0
  release(&vma_table.lock);
}

struct VMA*
vma_find(struct proc *p, uint64 va)
{
  for(int i = 0; i < NOFILE; i++){
    struct VMA *v = p->vma[i];
    if(v && v->addr <= va && va < v->addr + v->length)
      return v;
  }
  return 0;
}

static int
vma_writeback(struct proc *p, struct VMA *v, uint64 addr, uint64 length)
{
  if((v->prot & PROT_WRITE) == 0 || v->flags != MAP_SHARED)
    return 0;

  uint64 end = addr + length;
  for(uint64 va = PGROUNDDOWN(addr); va < end; va += PGSIZE){
    pte_t *pte = walk(p->pagetable, va, 0);
    if(pte == 0 || (*pte & PTE_V) == 0)
      continue;

    uint64 page_start = va < addr ? addr : va;
    uint64 page_end = va + PGSIZE;
    if(page_end > end)
      page_end = end;
    uint n = page_end - page_start;
    uint off = v->offset + page_start - v->addr;

    begin_op();
    ilock(v->file->ip);
    int written = writei(v->file->ip, 1, page_start, off, n);
    iunlock(v->file->ip);
    end_op();
    if(written != n)
      return -1;
  }
  return 0;
}

int
vma_unmap(struct proc *p, uint64 addr, uint64 length)
{
  if(length == 0 || (addr % PGSIZE) != 0)
    return -1;

  struct VMA *v = vma_find(p, addr);
  if(v == 0 || addr + length > v->addr + v->length)
    return -1;

  uint64 old_start = v->addr;
  uint64 old_end = v->addr + v->length;
  uint64 end = addr + length;
  if(addr != old_start && end != old_end)
    return -1; // middle-hole unmapping is intentionally unsupported.

  if(vma_writeback(p, v, addr, length) < 0)
    return -1;

  uint64 unmap_start = PGROUNDDOWN(addr);
  uint64 unmap_end = PGROUNDUP(end);
  uint64 npages = (unmap_end - unmap_start) / PGSIZE;
  uvmunmap(p->pagetable, unmap_start, npages, 1);
  u2kvmunmap(p->kpagetable, unmap_start, npages);

  if(addr == old_start && end == old_end){
    for(int i = 0; i < NOFILE; i++)
      if(p->vma[i] == v)
        p->vma[i] = 0;
    fileclose(v->file);
    vma_free(v);
  } else if(addr == old_start){
    v->addr = end;
    v->offset += length;
    v->length = old_end - end;
  } else {
    v->length = addr - old_start;
  }
  return 0;
}

void
vma_unmap_all(struct proc *p)
{
  for(int i = 0; i < NOFILE; i++){
    struct VMA *v = p->vma[i];
    if(v)
      vma_unmap(p, v->addr, v->length);
  }
}
