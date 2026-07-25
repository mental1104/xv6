#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "stat.h"
#include "file.h"
#include "proc.h"
#include "swap.h"

// vm.c is compiled through vmcopyin.h with these historical symbol names.
uint64 vm_legacy_walkaddr(pagetable_t, uint64);
int vm_legacy_uvmlazyalloc(struct proc*, uint64);
void vm_legacy_uvmunmap(pagetable_t, uint64, uint64, int);
void vm_legacy_uvmfree(pagetable_t, uint64);
int vm_legacy_uvmcopy(pagetable_t, pagetable_t, uint64);
int vm_legacy_copyout(pagetable_t, uint64, char*, uint64);

// This file contains copyin_new() and copyinstr_new(), the replacements for
// copyin and copyinstr in vm.c. It also interposes the public VM entry points
// that need to understand non-resident swap PTEs.

static struct stats {
  int ncopyin;
  int ncopyinstr;
} stats;

struct swap_state {
  struct spinlock lock;
  uchar used[NSWAP];
  uint64 page_outs;
  uint64 page_ins;
};

static struct swap_state swap_state;
static volatile int swap_state_ready;

/** Initialize the global slot allocator exactly once from ordinary process context. */
static void
ensure_swap_state(void)
{
  if(swap_state_ready == 2)
    return;

  if(__sync_bool_compare_and_swap(&swap_state_ready, 0, 1)){
    initlock(&swap_state.lock, "swap");
    __sync_synchronize();
    swap_state_ready = 2;
    return;
  }

  while(swap_state_ready != 2)
    ;
  __sync_synchronize();
}

/** Reserve one private backing-file slot before publishing a swapped PTE. */
static int
swap_slot_alloc(void)
{
  ensure_swap_state();
  acquire(&swap_state.lock);
  for(int slot = 0; slot < NSWAP; slot++){
    if(swap_state.used[slot] == 0){
      swap_state.used[slot] = 1;
      release(&swap_state.lock);
      return slot;
    }
  }
  release(&swap_state.lock);
  return -1;
}

/** Release a slot after page-in, unmap, process exit, or rollback. */
static void
swap_slot_free(int slot)
{
  if(slot < 0 || slot >= NSWAP)
    panic("swap slot");

  ensure_swap_state();
  acquire(&swap_state.lock);
  if(swap_state.used[slot] == 0)
    panic("swap double free");
  swap_state.used[slot] = 0;
  release(&swap_state.lock);
}

/**
 * Transfer one page between a kernel buffer and the preallocated swap file.
 *
 * The file is created by PID 1 before the first shell starts. Page-out changes
 * filesystem contents and therefore owns a transaction. Page-in is read-only
 * and deliberately avoids begin_op(), because a user buffer may fault while an
 * enclosing read/write syscall already owns a filesystem transaction.
 */
static int
swap_file_io(int write_page, int slot, char *page)
{
  if(slot < 0 || slot >= NSWAP || page == 0)
    return -1;

  int result = -1;
  uint64 offset = (uint64)slot * PGSIZE;

  if(write_page)
    begin_op();
  struct inode *ip = namei(SWAPFILE_PATH);
  if(ip != 0){
    ilock(ip);
    if(ip->type == T_FILE && ip->size >= (uint64)NSWAP * PGSIZE){
      int count;
      if(write_page)
        count = writei(ip, 0, (uint64)page, offset, PGSIZE);
      else
        count = readi(ip, 0, (uint64)page, offset, PGSIZE);
      if(count == PGSIZE)
        result = 0;
    }
    iunlockput(ip);
  }
  if(write_page)
    end_op();
  return result;
}

/** Count currently reserved slots while holding only the slot bitmap lock. */
static uint
swap_used_slots(void)
{
  uint used = 0;

  ensure_swap_state();
  acquire(&swap_state.lock);
  for(int slot = 0; slot < NSWAP; slot++)
    used += swap_state.used[slot] != 0;
  release(&swap_state.lock);
  return used;
}

/** Free the backing slot encoded in a non-resident leaf PTE. */
static void
swap_pte_release(pte_t pte)
{
  if(!PTE_IS_SWAPPED(pte))
    panic("swap pte release");
  swap_slot_free(PTE_TO_SWAP_SLOT(pte));
}

/**
 * Replace one current-process anonymous dynamic leaf with a disk-backed PTE.
 *
 * This is deliberately an explicit teaching operation rather than a global
 * replacement policy. It demonstrates the complete mechanism without
 * pretending xv6 has working-set estimation, watermarks, or an LRU scanner.
 */
static int
swap_out_page(struct proc *p, uint64 va)
{
  if(p == 0 || va >= p->sz || va >= USERMAX)
    return -1;

  uint64 va0 = PGROUNDDOWN(va);
  uint64 dynamic_start = PGROUNDUP(p->trapframe->sp);
  if(va0 < dynamic_start || vma_find(p, va0) != 0)
    return -1;

  pte_t *pte = walk(p->pagetable, va0, 0);
  if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 ||
     (*pte & (PTE_R | PTE_W | PTE_X)) == 0)
    return -1;

  int slot = swap_slot_alloc();
  if(slot < 0)
    return -1;

  pte_t resident = *pte;
  uint64 pa = PTE2PA(resident);
  if(swap_file_io(1, slot, (char *)pa) < 0){
    swap_slot_free(slot);
    return -1;
  }

  // The supervisor-only alias must disappear before the physical page is
  // released. The current process cannot run on another CPU simultaneously.
  u2kvmunmap(p->kpagetable, va0, 1);
  uint flags = PTE_FLAGS(resident) & ~PTE_V;
  *pte = SWAP_SLOT_TO_PTE(slot) | flags | PTE_SWAP;
  sfence_vma();
  kfree((void *)pa);

  acquire(&swap_state.lock);
  swap_state.page_outs++;
  release(&swap_state.lock);
  return 0;
}

/** Materialize one swapped current-process page and restore its kernel alias. */
static int
swap_in_page(struct proc *p, uint64 va)
{
  if(p == 0 || va >= p->sz || va >= USERMAX)
    return -1;

  uint64 va0 = PGROUNDDOWN(va);
  pte_t *pte = walk(p->pagetable, va0, 0);
  if(pte == 0 || !PTE_IS_SWAPPED(*pte))
    return -1;

  pte_t swapped = *pte;
  int slot = PTE_TO_SWAP_SLOT(swapped);
  char *mem = kalloc();
  if(mem == 0)
    return -1;
  if(swap_file_io(0, slot, mem) < 0){
    kfree(mem);
    return -1;
  }

  uint flags = PTE_FLAGS(swapped) & ~PTE_SWAP;
  *pte = PA2PTE((uint64)mem) | flags | PTE_V;
  if(u2kvmcopy(p->pagetable, p->kpagetable, va0, va0 + PGSIZE) < 0){
    *pte = swapped;
    sfence_vma();
    kfree(mem);
    return -1;
  }

  sfence_vma();
  swap_slot_free(slot);
  acquire(&swap_state.lock);
  swap_state.page_ins++;
  release(&swap_state.lock);
  return 0;
}

/** Duplicate a swapped page into a child-owned slot during fork(). */
static int
swap_pte_clone(pte_t source, pte_t *destination)
{
  if(!PTE_IS_SWAPPED(source) || destination == 0 || *destination != 0)
    return -1;

  int source_slot = PTE_TO_SWAP_SLOT(source);
  int destination_slot = swap_slot_alloc();
  if(destination_slot < 0)
    return -1;

  char *page = kalloc();
  if(page == 0){
    swap_slot_free(destination_slot);
    return -1;
  }
  if(swap_file_io(0, source_slot, page) < 0 ||
     swap_file_io(1, destination_slot, page) < 0){
    kfree(page);
    swap_slot_free(destination_slot);
    return -1;
  }
  kfree(page);

  *destination = SWAP_SLOT_TO_PTE(destination_slot) | PTE_FLAGS(source);
  return 0;
}

/** Fill a non-faulting page-state and global-counter snapshot for user tests. */
static int
swap_info_snapshot(struct proc *p, uint64 va, struct swap_info *info)
{
  if(p == 0 || info == 0 || va >= USERMAX)
    return -1;

  memset(info, 0, sizeof(*info));
  info->total_slots = NSWAP;
  info->used_slots = swap_used_slots();
  info->slot = -1;

  ensure_swap_state();
  acquire(&swap_state.lock);
  info->page_outs = swap_state.page_outs;
  info->page_ins = swap_state.page_ins;
  release(&swap_state.lock);

  pte_t *pte = walk(p->pagetable, PGROUNDDOWN(va), 0);
  if(pte != 0 && PTE_IS_SWAPPED(*pte)){
    info->page_state = SWAP_PAGE_SWAPPED;
    info->slot = PTE_TO_SWAP_SLOT(*pte);
  } else if(pte != 0 && (*pte & PTE_V) != 0 && (*pte & PTE_U) != 0 &&
            (*pte & (PTE_R | PTE_W | PTE_X)) != 0){
    info->page_state = SWAP_PAGE_RESIDENT;
  } else {
    info->page_state = SWAP_PAGE_UNMAPPED;
  }
  return 0;
}

/** Swap-aware public wrapper for address translation and lazy materialization. */
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  if(va >= USERMAX)
    return 0;

  pte_t *pte = walk(pagetable, va, 0);
  if(pte != 0 && PTE_IS_SWAPPED(*pte)){
    struct proc *p = myproc();
    if(p == 0 || p->pagetable != pagetable || swap_in_page(p, va) < 0)
      return 0;
  }
  return vm_legacy_walkaddr(pagetable, va);
}

/** Let the existing fault router treat swapped PTEs before ordinary lazy pages. */
int
uvmlazyalloc(struct proc *p, uint64 va)
{
  if(p != 0){
    pte_t *pte = walk(p->pagetable, PGROUNDDOWN(va), 0);
    if(pte != 0 && PTE_IS_SWAPPED(*pte))
      return swap_in_page(p, va);
  }
  return vm_legacy_uvmlazyalloc(p, va);
}

/** Remove resident leaves through vm.c and release non-resident slots here. */
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(uint64 a = va; a < va + npages * PGSIZE; a += PGSIZE){
    pte_t *pte = walk(pagetable, a, 0);
    if(pte != 0 && PTE_IS_SWAPPED(*pte)){
      if(do_free)
        swap_pte_release(*pte);
      *pte = 0;
    }
  }
  vm_legacy_uvmunmap(pagetable, va, npages, do_free);
}

/** Shrink sparse address spaces while treating swapped PTEs as owned leaves. */
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

/** Release every resident page, swapped slot, and page-table page on exit/exec. */
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  vm_legacy_uvmfree(pagetable, 0);
}

/** Preserve swapped private pages across fork using child-owned disk slots. */
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  if(vm_legacy_uvmcopy(old, new, sz) < 0)
    return -1;

  for(uint64 va = 0; va < sz; va += PGSIZE){
    pte_t *source = walk(old, va, 0);
    if(source == 0 || !PTE_IS_SWAPPED(*source))
      continue;

    pte_t *destination = walk(new, va, 1);
    if(destination == 0 || swap_pte_clone(*source, destination) < 0){
      uvmunmap(new, 0, PGROUNDUP(sz) / PGSIZE, 1);
      return -1;
    }
  }
  return 0;
}

/** Materialize swapped destinations before vm.c performs its ordinary COW copyout. */
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  if(dstva + len < dstva)
    return -1;
  for(uint64 va = PGROUNDDOWN(dstva); len > 0 && va < dstva + len;
      va += PGSIZE){
    if(walkaddr(pagetable, va) == 0)
      return -1;
  }
  return vm_legacy_copyout(pagetable, dstva, src, len);
}

int
statscopyin(char *buf, int sz)
{
  int n;
  n = snprintf(buf, sz, "copyin: %d\n", stats.ncopyin);
  n += snprintf(buf+n, sz, "copyinstr: %d\n", stats.ncopyinstr);
  return n;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  struct proc *p = myproc();
  if(p == 0 || pagetable != p->pagetable || srcva + len < srcva ||
     srcva + len > p->sz || srcva + len > USERMAX)
    return -1;

  // walkaddr() materializes both lazy and swapped pages; u2kvmcopy() restores
  // the supervisor-only high-address alias before the direct memmove.
  for(uint64 va = PGROUNDDOWN(srcva); va < srcva + len; va += PGSIZE)
    if(walkaddr(pagetable, va) == 0)
      return -1;

  memmove(dst, (void*)KUSERADDR(srcva), len);
  stats.ncopyin++;
  return 0;
}

// Copy a null-terminated user string to kernel memory, up to max bytes.
int
copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  struct proc *p = myproc();
  if(p == 0 || pagetable != p->pagetable || srcva >= p->sz ||
     srcva >= USERMAX)
    return -1;

  stats.ncopyinstr++;
  uint64 checked_page = MAXVA;
  for(uint64 i = 0; i < max && srcva + i >= srcva &&
      srcva + i < p->sz && srcva + i < USERMAX; i++){
    uint64 page = PGROUNDDOWN(srcva + i);
    if(page != checked_page){
      if(walkaddr(pagetable, page) == 0)
        return -1;
      checked_page = page;
    }
    dst[i] = *(char*)KUSERADDR(srcva + i);
    if(dst[i] == '\0')
      return 0;
  }
  return -1;
}

/** Explicit teaching syscall: evict one current-process anonymous user page. */
uint64
sys_swapout(void)
{
  uint64 va;
  if(argaddr(0, &va) < 0)
    return -1;
  return swap_out_page(myproc(), va);
}

/** Return global slot counters and the non-faulting state of one user page. */
uint64
sys_swapinfo(void)
{
  uint64 va;
  uint64 destination;
  struct swap_info info;
  struct proc *p = myproc();

  if(argaddr(0, &va) < 0 || argaddr(1, &destination) < 0)
    return -1;
  if(swap_info_snapshot(p, va, &info) < 0)
    return -1;
  if(copyout(p->pagetable, destination, (char *)&info, sizeof(info)) < 0)
    return -1;
  return 0;
}
