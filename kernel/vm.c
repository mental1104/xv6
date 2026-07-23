#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "vmcopyin.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "proc.h"
#include "fcntl.h"
#include "vma.h"

/*
 * The kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.
extern char trampoline[]; // trampoline.S

/*
 * Create a direct-map page table for the kernel.
 */
void
kvminit()
{
  kernel_pagetable = (pagetable_t)kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // UART registers.
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // VirtIO MMIO disk interface.
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT.
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC.
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // Map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // Map kernel data and the physical RAM the kernel will use.
  kvmmap((uint64)etext, (uint64)etext,
         PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // Map the trampoline at the highest kernel virtual address.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// Switch the hardware page-table register to the global kernel page table.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE corresponding to va. If alloc is non-zero,
// allocate any missing intermediate page-table pages.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  // The gap [MAXVA, KUSERBASE) is intentionally invalid. Addresses below
  // MAXVA serve normal user/kernel mappings; the high half is reserved for
  // the current process's supervisor-only user-page aliases.
  if(va >= MAXVA && va < KUSERBASE)
    panic("walk");

  for(int level = 2; level > 0; level--){
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V){
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pagetable_t)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Materialize one lazy user page and its supervisor-only kernel alias.
int
uvmlazyalloc(struct proc *p, uint64 va)
{
  uint64 va0 = PGROUNDDOWN(va);
  if(p == 0 || va >= USERMAX || va >= p->sz ||
     va0 < PGROUNDDOWN(p->trapframe->sp))
    return -1;
  if(vma_find(p, va))
    return -1;

  pte_t *pte = walk(p->pagetable, va0, 0);
  if(pte && (*pte & PTE_V))
    return 0;

  char *mem = kalloc();
  if(mem == 0)
    return -1;
  memset(mem, 0, PGSIZE);
  if(mappages(p->pagetable, va0, PGSIZE, (uint64)mem,
              PTE_W | PTE_X | PTE_R | PTE_U) < 0){
    kfree(mem);
    return -1;
  }
  if(u2kvmcopy(p->pagetable, p->kpagetable,
               va0, va0 + PGSIZE) < 0){
    // The user leaf is already committed, but an alias intermediate table may
    // fail to allocate under OOM. Roll back the user mapping so the caller
    // observes an ordinary allocation failure rather than a half-visible page.
    uvmunmap(p->pagetable, va0, 1, 1);
    return -1;
  }
  return 0;
}

// Look up a user virtual address and return its physical page address.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  if(va >= USERMAX)
    return 0;

  pte_t *pte = walk(pagetable, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0){
    struct proc *p = myproc();
    if(p == 0 || p->pagetable != pagetable || uvmlazyalloc(p, va) < 0)
      return 0;
    pte = walk(pagetable, va, 0);
  }
  if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
    return 0;
  return PTE2PA(*pte);
}

// Add a mapping to the global kernel page table during boot.
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Translate a kernel virtual address to a physical address.
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte = walk(kernel_pagetable, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0)
    panic("kvmpa");
  return PTE2PA(*pte) + off;
}

// Map every page touched by [va, va + size) to consecutive physical pages.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size,
         uint64 pa, int perm)
{
  uint64 a = PGROUNDDOWN(va);
  uint64 last = PGROUNDDOWN(va + size - 1);

  for(;;){
    pte_t *pte = walk(pagetable, a, 1);
    if(pte == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages mappings starting at page-aligned va. Missing leaves are
// tolerated because lazy allocation creates sparse address spaces.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(uint64 a = va; a < va + npages * PGSIZE; a += PGSIZE){
    pte_t *pte = walk(pagetable, a, 0);
    if(pte == 0 || (*pte & PTE_V) == 0)
      continue;
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free)
      kfree((void*)PTE2PA(*pte));
    *pte = 0;
  }
}

// Create an empty user page table.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable = (pagetable_t)kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the first process's initcode at user address zero.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  if(sz >= PGSIZE)
    panic("inituvm: more than a page");

  char *mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem,
           PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// Allocate user pages to grow a process from oldsz to newsz.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz < oldsz)
    return oldsz;
  if(newsz > USERMAX)
    return 0;

  oldsz = PGROUNDUP(oldsz);
  for(uint64 a = oldsz; a < newsz; a += PGSIZE){
    char *mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem,
                PTE_W | PTE_X | PTE_R | PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to shrink a process from oldsz to newsz.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    uint64 npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }
  return newsz;
}

// Recursively free page-table pages after every leaf has been removed.
void
freewalk(pagetable_t pagetable)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0){
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages and their page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Copy a parent's user page table into a child using copy-on-write leaves.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  uint64 i;

  for(i = 0; i < sz; i += PGSIZE){
    pte_t *pte = walk(old, i, 0);
    if(pte == 0 || (*pte & PTE_V) == 0)
      continue;

    uint64 pa = PTE2PA(*pte);
    uint flags = PTE_FLAGS(*pte);
    if(flags & PTE_W){
      flags = (flags | PTE_COW) & ~PTE_W;
      *pte = PA2PTE(pa) | flags;
    }
    increase_rc(pa);
    if(mappages(new, i, PGSIZE, pa, flags) != 0)
      goto err;
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// Mark a PTE invalid for user access, for example an exec stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy len bytes from kernel src to user virtual address dstva.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  while(len > 0){
    uint64 va0 = PGROUNDDOWN(dstva);
    cow_alloc(pagetable, va0);
    uint64 pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;

    uint64 n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void*)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy len bytes from user virtual address srcva to kernel dst.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  return copyin_new(pagetable, dst, srcva, len);
}

// Copy a NUL-terminated user string to kernel dst, up to max bytes.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  return copyinstr_new(pagetable, dst, srcva, max);
}

void
vmwalk(pagetable_t pagetable, int depth)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if(pte & PTE_V){
      uint64 pa = PTE2PA(pte);

      int temp = depth;
      while(temp--)
        printf(".. ");

      printf("..%d: pte %p pa %p\n", i, pte, pa);
      if(depth < 2)
        vmwalk((pagetable_t)pa, depth + 1);
    }
  }
}

void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", pagetable);
  vmwalk(pagetable, 0);
}

void
kvmmapkern(pagetable_t pagetable, uint64 va, uint64 pa,
           uint64 sz, int perm)
{
  if(mappages(pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create a per-process kernel page table. Low-half direct mappings are shared
// with the global table; the high-half alias subtree remains process-private.
pagetable_t
kvmcreate()
{
  pagetable_t pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  for(int i = 0; i < PX(2, KUSERBASE); i++)
    pagetable[i] = kernel_pagetable[i];
  return pagetable;
}

// Free a private kernel alias subtree. Alias leaves borrow user physical pages,
// so this releases only page-table pages, never leaf physical memory.
static void
kvmfreewalk(pagetable_t pagetable, int level)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) == 0)
      continue;
    if(level > 0 && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
      kvmfreewalk((pagetable_t)PTE2PA(pte), level - 1);
    else if(level > 0)
      panic("kvmfreewalk: leaf");
    pagetable[i] = 0;
  }
  kfree((void*)pagetable);
}

void
kvmfree(pagetable_t kpagetable)
{
  for(int i = PX(2, KUSERBASE); i < 512; i++){
    pte_t pte = kpagetable[i];
    if((pte & PTE_V) == 0)
      continue;
    if(pte & (PTE_R | PTE_W | PTE_X))
      panic("kvmfree: root leaf");
    kvmfreewalk((pagetable_t)PTE2PA(pte), 1);
    kpagetable[i] = 0;
  }
  kfree((void*)kpagetable);
}

/**
 * Mirror existing user leaves into a process kernel page table's high-address
 * supervisor-only alias window.
 *
 * @param pagetable User page table whose valid leaves supply physical pages and
 *        permissions.
 * @param kpagetable Process-private kernel page table that owns alias page-table
 *        pages but not the aliased physical pages.
 * @param oldsz First byte of the range to inspect.
 * @param newsz One-past-the-end byte of the range; must not exceed USERMAX.
 * @return 0 when every valid user leaf was mirrored, or -1 for an invalid range
 *         or alias page-table allocation failure. The caller must roll back any
 *         user mappings or temporary process state not yet committed.
 */
int
u2kvmcopy(pagetable_t pagetable, pagetable_t kpagetable,
          uint64 oldsz, uint64 newsz)
{
  if(newsz < oldsz || newsz > USERMAX)
    return -1;

  uint64 start = PGROUNDDOWN(oldsz);
  for(uint64 a = start; a < newsz; a += PGSIZE){
    pte_t *from = walk(pagetable, a, 0);
    if(from == 0 || (*from & PTE_V) == 0)
      continue;

    pte_t *to = walk(kpagetable, KUSERADDR(a), 1);
    if(to == 0){
      sfence_vma();
      return -1;
    }
    uint flags = PTE_FLAGS(*from) & ~PTE_U;
    *to = PA2PTE(PTE2PA(*from)) | flags;
  }
  sfence_vma();
  return 0;
}

void
u2kvmunmap(pagetable_t kpagetable, uint64 va, uint64 npages)
{
  if(npages == 0)
    return;
  if(va >= USERMAX || npages > (USERMAX - va) / PGSIZE)
    panic("u2kvmunmap: range");
  uvmunmap(kpagetable, KUSERADDR(va), npages, 0);
  sfence_vma();
}
