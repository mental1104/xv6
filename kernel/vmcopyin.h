struct proc;

uint64 vm_legacy_walkaddr(pagetable_t, uint64);
int vm_legacy_uvmlazyalloc(struct proc *, uint64);
void vm_legacy_uvmunmap(pagetable_t, uint64, uint64, int);
uint64 vm_legacy_uvmdealloc(pagetable_t, uint64, uint64);
void vm_legacy_uvmfree(pagetable_t, uint64);
int vm_legacy_uvmcopy(pagetable_t, pagetable_t, uint64);
int vm_legacy_copyout(pagetable_t, uint64, char *, uint64);

// vm.c includes this header after defs.h. Rename only that translation unit's
// public VM entry points so vmcopyin.c can provide swap-aware wrappers without
// duplicating the existing lazy-allocation, COW, and page-table implementation.
#define walkaddr vm_legacy_walkaddr
#define uvmlazyalloc vm_legacy_uvmlazyalloc
#define uvmunmap vm_legacy_uvmunmap
#define uvmdealloc vm_legacy_uvmdealloc
#define uvmfree vm_legacy_uvmfree
#define uvmcopy vm_legacy_uvmcopy
#define copyout vm_legacy_copyout

int copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len);
int copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max);
