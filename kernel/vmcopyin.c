#include "param.h"
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"

//
// This file contains copyin_new() and copyinstr_new(), the
// replacements for copyin and coyinstr in vm.c.
//

static struct stats {
  int ncopyin;
  int ncopyinstr;
} stats;

int
statscopyin(char *buf, int sz) {
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
  if(p == 0 || pagetable != p->pagetable || srcva + len < srcva || srcva + len > p->sz)
    return -1;

  for(uint64 va = PGROUNDDOWN(srcva); va < srcva + len; va += PGSIZE)
    if(walkaddr(pagetable, va) == 0)
      return -1;

  memmove(dst, (void*)srcva, len);
  stats.ncopyin++;
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  struct proc *p = myproc();
  if(p == 0 || pagetable != p->pagetable || srcva >= p->sz)
    return -1;

  stats.ncopyinstr++;
  uint64 checked_page = MAXVA;
  for(uint64 i = 0; i < max && srcva + i < p->sz; i++){
    uint64 page = PGROUNDDOWN(srcva + i);
    if(page != checked_page){
      if(walkaddr(pagetable, page) == 0)
        return -1;
      checked_page = page;
    }
    dst[i] = *(char*)(srcva + i);
    if(dst[i] == '\0')
      return 0;
  }
  return -1;
}
