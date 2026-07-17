#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "vmstat.h"

#ifndef PTE_COW
#define PTE_COW (1L << 8)
#endif

extern struct proc proc[NPROC];

struct mapping_summary {
  uint64 scanned;
  uint64 present;
  uint64 private_pages;
  uint64 shared_pages;
  uint64 cow_pages;
};

struct vmstat_state {
  struct spinlock lock;
  int initialized;
  int active;
  int root_pid;
  int child_pid;
  uint64 range_start;
  uint64 range_end;
  uint64 begin_total_present;
  uint64 begin_range_present;
  struct vmstat_snapshot snapshot;
};

static struct vmstat_state state;

static void
vmstat_init_once(void)
{
  if(state.initialized)
    return;
  initlock(&state.lock, "vmstat");
  state.initialized = 1;
}

static int
user_leaf(pagetable_t pagetable, uint64 va, pte_t *value)
{
  pte_t *pte = walk(pagetable, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
    return 0;
  if((*pte & (PTE_R | PTE_W | PTE_X)) == 0)
    return 0;
  if(value)
    *value = *pte;
  return 1;
}

static uint64
count_present(pagetable_t pagetable, uint64 sz, uint64 start, uint64 end)
{
  uint64 count = 0;
  uint64 first = PGROUNDDOWN(start);
  uint64 limit = PGROUNDUP(end);

  if(limit > PGROUNDUP(sz))
    limit = PGROUNDUP(sz);
  for(uint64 va = first; va < limit; va += PGSIZE)
    if(user_leaf(pagetable, va, 0))
      count++;
  return count;
}

static void
scan_relation(struct proc *parent, struct proc *child,
              uint64 start, uint64 end, struct mapping_summary *summary)
{
  memset(summary, 0, sizeof(*summary));

  uint64 first = PGROUNDDOWN(start);
  uint64 limit = PGROUNDUP(end);
  if(limit > PGROUNDUP(parent->sz))
    limit = PGROUNDUP(parent->sz);

  for(uint64 va = first; va < limit; va += PGSIZE){
    pte_t parent_pte;
    pte_t child_pte;

    summary->scanned++;
    if(!user_leaf(parent->pagetable, va, &parent_pte))
      continue;
    summary->present++;
    if(!user_leaf(child->pagetable, va, &child_pte))
      continue;

    if(PTE2PA(parent_pte) == PTE2PA(child_pte))
      summary->shared_pages++;
    else
      summary->private_pages++;

    if((parent_pte & PTE_COW) || (child_pte & PTE_COW))
      summary->cow_pages++;
  }
}

static struct proc *
find_proc_locked(int pid)
{
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid && p->state != UNUSED)
      return p;
    release(&p->lock);
  }
  return 0;
}

static void
record_fork_counts(struct vmstat_counts *counts,
                   const struct mapping_summary *summary)
{
  counts->fork_va_scan_pages = summary->scanned;
  counts->fork_present_pages = summary->present;
  counts->fork_eager_copy_pages = summary->private_pages;
  counts->fork_shared_map_pages = summary->shared_pages;
  counts->fork_cow_mark_pages = summary->cow_pages;
}

static uint64
additional_private_pages(uint64 current_private, uint64 fork_private)
{
  if(current_private <= fork_private)
    return 0;
  return current_private - fork_private;
}

static int
vmstat_begin(struct proc *caller, uint64 start, uint64 end)
{
  if(end <= start || end > MAXVA)
    return -1;

  acquire(&state.lock);
  state.active = 1;
  state.root_pid = caller->pid;
  state.child_pid = 0;
  state.range_start = start;
  state.range_end = end;
  memset(&state.snapshot, 0, sizeof(state.snapshot));
  state.snapshot.abi_version = VMSTAT_ABI_VERSION;
  state.snapshot.range_start = start;
  state.snapshot.range_end = end;
  state.begin_total_present = count_present(caller->pagetable, caller->sz, 0, caller->sz);
  state.begin_range_present = count_present(caller->pagetable, caller->sz, start, end);
  release(&state.lock);
  return 0;
}

static int
vmstat_sample_sbrk(struct proc *caller, uint64 oldsz, uint64 newsz)
{
  if(newsz < oldsz)
    return -1;

  acquire(&state.lock);
  if(!state.active || caller->pid != state.root_pid){
    release(&state.lock);
    return -1;
  }

  uint64 grow_start = PGROUNDUP(oldsz);
  uint64 grow_end = PGROUNDUP(newsz);
  uint64 total_pages = 0;
  uint64 range_pages = 0;
  uint64 total_eager = 0;
  uint64 range_eager = 0;

  for(uint64 va = grow_start; va < grow_end; va += PGSIZE){
    total_pages++;
    if(user_leaf(caller->pagetable, va, 0))
      total_eager++;
    if(va >= PGROUNDDOWN(state.range_start) && va < PGROUNDUP(state.range_end)){
      range_pages++;
      if(user_leaf(caller->pagetable, va, 0))
        range_eager++;
    }
  }

  state.snapshot.total.virtual_grow_pages += total_pages;
  state.snapshot.range.virtual_grow_pages += range_pages;
  state.snapshot.total.sbrk_eager_alloc_pages += total_eager;
  state.snapshot.range.sbrk_eager_alloc_pages += range_eager;
  release(&state.lock);
  return 0;
}

static int
vmstat_sample_fork(struct proc *caller, int child_pid)
{
  acquire(&state.lock);
  if(!state.active || caller->pid != state.root_pid || child_pid <= 0){
    release(&state.lock);
    return -1;
  }

  struct proc *child = find_proc_locked(child_pid);
  if(child == 0){
    release(&state.lock);
    return -1;
  }

  struct mapping_summary total;
  struct mapping_summary range;
  scan_relation(caller, child, 0, caller->sz, &total);
  scan_relation(caller, child, state.range_start, state.range_end, &range);
  record_fork_counts(&state.snapshot.total, &total);
  record_fork_counts(&state.snapshot.range, &range);
  state.child_pid = child_pid;

  release(&child->lock);
  release(&state.lock);
  return 0;
}

static int
vmstat_sample_child(struct proc *caller)
{
  acquire(&state.lock);
  if(!state.active || caller->pid != state.child_pid){
    release(&state.lock);
    return -1;
  }

  struct proc *root = find_proc_locked(state.root_pid);
  if(root == 0){
    release(&state.lock);
    return -1;
  }

  struct mapping_summary total;
  struct mapping_summary range;
  scan_relation(root, caller, 0, root->sz, &total);
  scan_relation(root, caller, state.range_start, state.range_end, &range);

  state.snapshot.total.cow_copy_pages =
    additional_private_pages(total.private_pages,
                             state.snapshot.total.fork_eager_copy_pages);
  state.snapshot.range.cow_copy_pages =
    additional_private_pages(range.private_pages,
                             state.snapshot.range.fork_eager_copy_pages);

  release(&root->lock);
  release(&state.lock);
  return 0;
}

static int
vmstat_end(struct proc *caller, uint64 user_dst)
{
  struct vmstat_snapshot snapshot;

  acquire(&state.lock);
  if(!state.active || caller->pid != state.root_pid || user_dst == 0){
    release(&state.lock);
    return -1;
  }

  uint64 total_present = count_present(caller->pagetable, caller->sz, 0, caller->sz);
  uint64 range_present = count_present(caller->pagetable, caller->sz,
                                       state.range_start, state.range_end);
  state.snapshot.total.final_present_pages = total_present;
  state.snapshot.range.final_present_pages = range_present;

  uint64 total_known = state.begin_total_present +
                       state.snapshot.total.sbrk_eager_alloc_pages;
  uint64 range_known = state.begin_range_present +
                       state.snapshot.range.sbrk_eager_alloc_pages;
  if(total_present > total_known)
    state.snapshot.total.lazy_materialized_pages = total_present - total_known;
  if(range_present > range_known)
    state.snapshot.range.lazy_materialized_pages = range_present - range_known;

  snapshot = state.snapshot;
  state.active = 0;
  release(&state.lock);

  if(copyout(caller->pagetable, user_dst, (char *)&snapshot, sizeof(snapshot)) < 0)
    return -1;
  return 0;
}

uint64
sys_vmstat(void)
{
  int op;
  uint64 arg0;
  uint64 arg1;
  uint64 user_dst;

  vmstat_init_once();
  if(argint(0, &op) < 0 ||
     argaddr(1, &arg0) < 0 ||
     argaddr(2, &arg1) < 0 ||
     argaddr(3, &user_dst) < 0)
    return -1;

  struct proc *caller = myproc();
  switch(op){
  case VMSTAT_BEGIN:
    return vmstat_begin(caller, arg0, arg1);
  case VMSTAT_SAMPLE_SBRK:
    return vmstat_sample_sbrk(caller, arg0, arg1);
  case VMSTAT_SAMPLE_FORK:
    return vmstat_sample_fork(caller, (int)arg0);
  case VMSTAT_SAMPLE_CHILD:
    return vmstat_sample_child(caller);
  case VMSTAT_END:
    return vmstat_end(caller, user_dst);
  default:
    return -1;
  }
}
