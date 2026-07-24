#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "sched.h"
#include "schedstat.h"
#include "schedtrace.h"

#ifndef SCHED_POLICY
#define SCHED_POLICY SCHED_POLICY_RR
#endif

#define RR_QUANTUM             2
#define MLFQ_LEVELS            3
#define MLFQ_BOOST_INTERVAL   64
#define CFS_MIN_GRANULARITY    2
#define CFS_NICE_0_WEIGHT   1024
#define CFS_VRUNTIME_SCALE  1024
#define DEFAULT_BURST_HINT  ((uint64)1 << 60)

#define offset_of(type, member) ((uint64)&((type *)0)->member)
#define container_of(ptr, type, member) \
  ((type *)((char *)(ptr) - offset_of(type, member)))

struct sched_list {
  struct proc *head;
  struct proc *tail;
  int count;
};

struct sched_runqueue {
  struct spinlock lock;
  struct sched_list queue;
  struct sched_list mlfq[MLFQ_LEVELS];
  struct rb_root_cached cfs;
  uint64 enqueue_seq;
  uint64 clock;
  uint64 boost_epoch;
  uint64 last_boost;
  uint64 min_vruntime;
};

struct sched_policy_ops {
  const char *name;
  void (*enqueue)(struct proc *p);
  void (*dequeue)(struct proc *p);
  struct proc *(*pick_next)(void);
  void (*tick)(struct proc *p);
  int (*should_preempt)(struct proc *p);
  void (*dispatch)(struct proc *p);
};

static struct sched_runqueue runq;
extern struct proc proc[NPROC];
extern void legacy_procinit(void);
extern void legacy_yield(void);

static void list_push_tail(struct sched_list *list, struct proc *p);
static void list_remove(struct sched_list *list, struct proc *p);
static struct proc *list_pop_head(struct sched_list *list);
static void sync_runqueue(void);
static struct proc *reserve_next(void);

static void
list_push_tail(struct sched_list *list, struct proc *p)
{
  p->sched.prev = list->tail;
  p->sched.next = 0;
  if(list->tail)
    list->tail->sched.next = p;
  else
    list->head = p;
  list->tail = p;
  list->count++;
}

static void
list_remove(struct sched_list *list, struct proc *p)
{
  if(p->sched.prev)
    p->sched.prev->sched.next = p->sched.next;
  else
    list->head = p->sched.next;
  if(p->sched.next)
    p->sched.next->sched.prev = p->sched.prev;
  else
    list->tail = p->sched.prev;
  p->sched.prev = 0;
  p->sched.next = 0;
  if(list->count <= 0)
    panic("sched list count");
  list->count--;
}

static struct proc *
list_pop_head(struct sched_list *list)
{
  struct proc *p = list->head;
  if(p)
    list_remove(list, p);
  return p;
}

static uint64
burst_key(struct proc *p)
{
  return p->sched.burst_hint ? p->sched.burst_hint : DEFAULT_BURST_HINT;
}

static uint64
remaining_key(struct proc *p)
{
  if(p->sched.burst_hint == 0)
    return DEFAULT_BURST_HINT;
  return p->sched.remaining_hint ? p->sched.remaining_hint : 1;
}

static void
list_enqueue_common(struct proc *p)
{
  list_push_tail(&runq.queue, p);
}

static void
list_dequeue_common(struct proc *p)
{
  list_remove(&runq.queue, p);
}

static struct proc *
pick_fifo(void)
{
  return list_pop_head(&runq.queue);
}

static struct proc *
pick_shortest(int remaining)
{
  struct proc *p;
  struct proc *best = 0;
  uint64 best_key = 0;

  for(p = runq.queue.head; p; p = p->sched.next){
    uint64 key = remaining ? remaining_key(p) : burst_key(p);
    if(best == 0 || key < best_key ||
       (key == best_key && p->sched.enqueue_seq < best->sched.enqueue_seq)){
      best = p;
      best_key = key;
    }
  }
  if(best)
    list_remove(&runq.queue, best);
  return best;
}

static struct proc *
pick_sjf(void)
{
  return pick_shortest(0);
}

static struct proc *
pick_stcf(void)
{
  return pick_shortest(1);
}

static void
basic_tick(struct proc *p)
{
  (void)p;
}

static int
never_preempt(struct proc *p)
{
  (void)p;
  return 0;
}

static int
rr_preempt(struct proc *p)
{
  if(p->sched.slice_ticks >= RR_QUANTUM){
    p->sched.pending_stop_reason = SCHEDTRACE_REASON_RR_QUANTUM;
    return 1;
  }
  return 0;
}

static int
stcf_preempt(struct proc *p)
{
  struct proc *best;
  uint64 current;
  uint64 candidate;

  if(runq.queue.head == 0)
    return 0;
  best = runq.queue.head;
  for(struct proc *q = best->sched.next; q; q = q->sched.next)
    if(remaining_key(q) < remaining_key(best))
      best = q;
  current = remaining_key(p);
  candidate = remaining_key(best);
  if(candidate < current){
    p->sched.pending_stop_reason = SCHEDTRACE_REASON_STCF_SHORTER_TASK;
    return 1;
  }
  return 0;
}

static void
basic_dispatch(struct proc *p)
{
  p->sched.slice_ticks = 0;
}

static const int mlfq_quantum[MLFQ_LEVELS] = {1, 2, 4};

static void
mlfq_enqueue(struct proc *p)
{
  int level = p->sched.mlfq_level;
  if(level < 0 || level >= MLFQ_LEVELS)
    level = 0;
  p->sched.mlfq_level = level;
  list_push_tail(&runq.mlfq[level], p);
}

static void
mlfq_dequeue(struct proc *p)
{
  int level = p->sched.mlfq_level;
  if(level < 0 || level >= MLFQ_LEVELS)
    panic("mlfq level");
  list_remove(&runq.mlfq[level], p);
}

static struct proc *
mlfq_pick(void)
{
  for(int level = 0; level < MLFQ_LEVELS; level++)
    if(runq.mlfq[level].head)
      return list_pop_head(&runq.mlfq[level]);
  return 0;
}

static void
mlfq_boost_locked(void)
{
  struct proc *p;

  runq.boost_epoch++;
  for(p = runq.mlfq[0].head; p; p = p->sched.next){
    p->sched.mlfq_used = 0;
    p->sched.mlfq_epoch = runq.boost_epoch;
  }
  for(int level = 1; level < MLFQ_LEVELS; level++){
    while((p = list_pop_head(&runq.mlfq[level])) != 0){
      p->sched.mlfq_level = 0;
      p->sched.mlfq_used = 0;
      p->sched.mlfq_epoch = runq.boost_epoch;
      list_push_tail(&runq.mlfq[0], p);
    }
  }
  runq.last_boost = runq.clock;
  schedtrace_record_boost(runq.clock, runq.boost_epoch);
}

static void
mlfq_tick(struct proc *p)
{
  if(p->sched.mlfq_epoch != runq.boost_epoch){
    p->sched.mlfq_epoch = runq.boost_epoch;
    p->sched.mlfq_level = 0;
    p->sched.mlfq_used = 0;
  }
  p->sched.mlfq_used++;
  if(runq.clock - runq.last_boost >= MLFQ_BOOST_INTERVAL)
    mlfq_boost_locked();
  if(p->sched.mlfq_used >= (uint64)mlfq_quantum[p->sched.mlfq_level]){
    if(p->sched.mlfq_level + 1 < MLFQ_LEVELS)
      p->sched.mlfq_level++;
    p->sched.mlfq_used = 0;
    p->sched.force_preempt = 1;
    p->sched.pending_stop_reason = SCHEDTRACE_REASON_MLFQ_QUANTUM;
  }
}

static int
mlfq_preempt(struct proc *p)
{
  if(p->sched.force_preempt)
    return 1;
  for(int level = 0; level < p->sched.mlfq_level; level++)
    if(runq.mlfq[level].head){
      p->sched.pending_stop_reason = SCHEDTRACE_REASON_MLFQ_HIGHER_QUEUE;
      return 1;
    }
  return 0;
}

static void
mlfq_dispatch(struct proc *p)
{
  p->sched.slice_ticks = 0;
  p->sched.force_preempt = 0;
}

static int
cfs_less(struct proc *a, struct proc *b)
{
  if(a->sched.vruntime != b->sched.vruntime)
    return a->sched.vruntime < b->sched.vruntime;
  return a->pid < b->pid;
}

static struct proc *
rb_proc(struct rb_node *node)
{
  return container_of(node, struct proc, sched.rb);
}

static void
cfs_enqueue(struct proc *p)
{
  struct rb_node **link = &runq.cfs.root;
  struct rb_node *parent = 0;
  int leftmost = 1;

  if(p->sched.vruntime < runq.min_vruntime)
    p->sched.vruntime = runq.min_vruntime;

  while(*link){
    struct proc *current = rb_proc(*link);
    parent = *link;
    if(cfs_less(p, current))
      link = &parent->left;
    else {
      leftmost = 0;
      link = &parent->right;
    }
  }
  rb_link_node(&p->sched.rb, parent, link);
  rb_insert_color_cached(&runq.cfs, &p->sched.rb, leftmost);
}

static void
cfs_dequeue(struct proc *p)
{
  rb_erase_cached(&runq.cfs, &p->sched.rb);
}

static struct proc *
cfs_pick(void)
{
  struct rb_node *node = rb_first(&runq.cfs);
  struct proc *p;
  if(node == 0)
    return 0;
  p = rb_proc(node);
  rb_erase_cached(&runq.cfs, node);
  return p;
}

static void
cfs_update_min_locked(struct proc *current)
{
  uint64 candidate = current->sched.vruntime;
  struct rb_node *leftmost = rb_first(&runq.cfs);
  if(leftmost){
    struct proc *left = rb_proc(leftmost);
    if(left->sched.vruntime < candidate)
      candidate = left->sched.vruntime;
  }
  if(candidate > runq.min_vruntime)
    runq.min_vruntime = candidate;
}

static void
cfs_tick(struct proc *p)
{
  uint64 delta;
  int weight = p->sched.weight;
  if(weight <= 0)
    weight = CFS_NICE_0_WEIGHT;
  delta = ((uint64)CFS_NICE_0_WEIGHT * CFS_VRUNTIME_SCALE) /
          (uint64)weight;
  if(delta == 0)
    delta = 1;
  p->sched.vruntime += delta;
  cfs_update_min_locked(p);
}

static int
cfs_preempt(struct proc *p)
{
  struct rb_node *node;
  struct proc *left;
  if(p->sched.slice_ticks < CFS_MIN_GRANULARITY)
    return 0;
  node = rb_first(&runq.cfs);
  if(node == 0)
    return 0;
  left = rb_proc(node);
  if(cfs_less(left, p)){
    p->sched.pending_stop_reason = SCHEDTRACE_REASON_CFS_LOWER_VRUNTIME;
    return 1;
  }
  return 0;
}

static void
cfs_dispatch(struct proc *p)
{
  p->sched.slice_ticks = 0;
}

static const struct sched_policy_ops rr_ops = {
  "rr", list_enqueue_common, list_dequeue_common, pick_fifo,
  basic_tick, rr_preempt, basic_dispatch
};
static const struct sched_policy_ops fifo_ops = {
  "fifo", list_enqueue_common, list_dequeue_common, pick_fifo,
  basic_tick, never_preempt, basic_dispatch
};
static const struct sched_policy_ops sjf_ops = {
  "sjf", list_enqueue_common, list_dequeue_common, pick_sjf,
  basic_tick, never_preempt, basic_dispatch
};
static const struct sched_policy_ops stcf_ops = {
  "stcf", list_enqueue_common, list_dequeue_common, pick_stcf,
  basic_tick, stcf_preempt, basic_dispatch
};
static const struct sched_policy_ops mlfq_ops = {
  "mlfq", mlfq_enqueue, mlfq_dequeue, mlfq_pick,
  mlfq_tick, mlfq_preempt, mlfq_dispatch
};
static const struct sched_policy_ops cfs_ops = {
  "cfs", cfs_enqueue, cfs_dequeue, cfs_pick,
  cfs_tick, cfs_preempt, cfs_dispatch
};

static const struct sched_policy_ops *
policy_ops(void)
{
  switch(SCHED_POLICY){
  case SCHED_POLICY_FIFO: return &fifo_ops;
  case SCHED_POLICY_SJF: return &sjf_ops;
  case SCHED_POLICY_STCF: return &stcf_ops;
  case SCHED_POLICY_MLFQ: return &mlfq_ops;
  case SCHED_POLICY_CFS: return &cfs_ops;
  case SCHED_POLICY_RR:
  default: return &rr_ops;
  }
}

const char *
sched_policy_name(void)
{
  return policy_ops()->name;
}

static void
entity_reset(struct proc *p)
{
  memset(&p->sched, 0, sizeof(p->sched));
  rb_node_init(&p->sched.rb);
  p->sched.generation = p->pid;
  p->sched.weight = CFS_NICE_0_WEIGHT;
  p->sched.mlfq_epoch = runq.boost_epoch;
}

static void
remove_locked(struct proc *p)
{
  if(!p->sched.on_rq)
    return;
  policy_ops()->dequeue(p);
  p->sched.on_rq = 0;
}

static void
enqueue_locked(struct proc *p)
{
  if(p->sched.on_rq || p->sched.reserved)
    return;
  p->sched.enqueue_seq = ++runq.enqueue_seq;
  policy_ops()->enqueue(p);
  p->sched.on_rq = 1;
}

static void
sync_runqueue(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    acquire(&runq.lock);

    if(p->pid != 0 && p->sched.generation != p->pid){
      if(p->sched.on_rq)
        remove_locked(p);
      entity_reset(p);
    }

    if(p->state == RUNNABLE){
      enqueue_locked(p);
    } else if(p->sched.on_rq){
      remove_locked(p);
    }

    release(&runq.lock);
    release(&p->lock);
  }
}

static struct proc *
reserve_next(void)
{
  struct proc *p;

  acquire(&runq.lock);
  p = policy_ops()->pick_next();
  if(p){
    p->sched.on_rq = 0;
    p->sched.reserved = 1;
  }
  release(&runq.lock);
  return p;
}

void
schedinit(void)
{
  initlock(&runq.lock, "sched_rq");
  memset(&runq.queue, 0, sizeof(runq.queue));
  memset(runq.mlfq, 0, sizeof(runq.mlfq));
  rb_root_init(&runq.cfs);
  runq.enqueue_seq = 0;
  runq.clock = 0;
  runq.boost_epoch = 1;
  runq.last_boost = 0;
  runq.min_vruntime = 0;
  schedtrace_init();
  printf("scheduler: %s\n", (char *)sched_policy_name());
}

// Keep proc.c unchanged: the Makefile renames its procinit/scheduler/yield
// definitions, and these wrappers install the policy-aware implementation.
void
procinit(void)
{
  legacy_procinit();
  schedinit();
}

void
scheduler(void)
{
  struct cpu *c = mycpu();
  c->proc = 0;

  for(;;){
    struct proc *p;
    intr_on();
    sync_runqueue();
    p = reserve_next();
    if(p == 0){
      kvminithart();
      asm volatile("wfi");
      continue;
    }

    acquire(&p->lock);
    acquire(&runq.lock);
    p->sched.reserved = 0;
    release(&runq.lock);

    if(p->state != RUNNABLE){
      release(&p->lock);
      continue;
    }

    p->state = RUNNING;
    p->sched.dispatches++;
    p->sched.pending_stop_reason = SCHEDTRACE_REASON_NONE;
    p->sched.last_clock = runq.clock;
    policy_ops()->dispatch(p);
    c->proc = p;
    schedtrace_record_start(p);

    w_satp(MAKE_SATP(p->kpagetable));
    sfence_vma();
    swtch(&c->context, &p->context);

    // swtch() 返回时仍持有 p->lock。先切回全局内核页表，再允许另一 CPU
    // 在 wait()/freeproc() 中取得该锁并释放 p->kpagetable。
    kvminithart();

    int stop_reason = p->sched.pending_stop_reason;
    if(p->killed)
      stop_reason = SCHEDTRACE_REASON_KILLED;
    else if(p->state == SLEEPING)
      stop_reason = SCHEDTRACE_REASON_SLEEP;
    else if(p->state == ZOMBIE)
      stop_reason = SCHEDTRACE_REASON_EXIT;
    else if(p->state == RUNNABLE && stop_reason == SCHEDTRACE_REASON_NONE)
      stop_reason = SCHEDTRACE_REASON_VOLUNTARY_YIELD;
    else if(stop_reason == SCHEDTRACE_REASON_NONE)
      stop_reason = SCHEDTRACE_REASON_UNKNOWN;
    schedtrace_record_stop(p, stop_reason);
    p->sched.pending_stop_reason = SCHEDTRACE_REASON_NONE;
    c->proc = 0;
    release(&p->lock);
  }
}

void
yield(void)
{
  struct proc *p = myproc();
  if(p && p->sched.pending_stop_reason == SCHEDTRACE_REASON_NONE)
    p->sched.pending_stop_reason = SCHEDTRACE_REASON_VOLUNTARY_YIELD;
  legacy_yield();
}

void
sched_timer_yield(void)
{
  struct proc *p = myproc();
  int preempt;

  if(p == 0 || p->state != RUNNING)
    return;

  // A wakeup may have made a shorter or higher-priority task RUNNABLE
  // since the last trip through scheduler(). Synchronize before applying
  // the policy-specific preemption rule.
  sync_runqueue();

  acquire(&runq.lock);
  runq.clock++;
  p->sched.last_clock = runq.clock;
  p->sched.runtime_ticks++;
  p->sched.slice_ticks++;
  if(p->sched.burst_hint && p->sched.remaining_hint)
    p->sched.remaining_hint--;
  policy_ops()->tick(p);
  preempt = policy_ops()->should_preempt(p);
  release(&runq.lock);

  if(preempt)
    legacy_yield();
}

int
sched_set_hint(int ticks_hint)
{
  struct proc *p = myproc();
  if(ticks_hint <= 0)
    return -1;
  p->sched.burst_hint = (uint64)ticks_hint;
  p->sched.remaining_hint = (uint64)ticks_hint;
  return 0;
}

int
sched_set_weight(int weight)
{
  struct proc *p = myproc();
  if(weight < 1 || weight > 4096)
    return -1;
  p->sched.weight = weight;
  return 0;
}

int
sched_get_stats(struct sched_stats *stats)
{
  struct proc *p = myproc();
  stats->policy = SCHED_POLICY;
  stats->pid = p->pid;
  stats->mlfq_level = p->sched.mlfq_level;
  stats->weight = p->sched.weight;
  stats->runtime_ticks = p->sched.runtime_ticks;
  stats->dispatches = p->sched.dispatches;
  stats->burst_hint = p->sched.burst_hint;
  stats->remaining_hint = p->sched.remaining_hint;
  stats->vruntime = p->sched.vruntime;
  stats->mlfq_epoch = p->sched.mlfq_epoch;
  return 0;
}
