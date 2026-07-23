#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "schedstat.h"
#include "schedtrace.h"

#ifndef XV6_CPUS
#define XV6_CPUS NCPU
#endif

extern struct proc proc[NPROC];

struct schedtrace_filter {
  int pid;
  int generation;
};

struct schedtrace_buffer {
  struct spinlock lock;
  int active;
  int dropped;
  int events;
  unsigned long next_seq;
  struct schedtrace_filter filters[SCHEDTRACE_MAX_FILTERS];
  struct schedtrace_event events_buffer[SCHEDTRACE_MAX_EVENTS];
};

static struct schedtrace_buffer trace;

/**
 * schedtrace_init 初始化固定容量调度轨迹缓冲。
 *
 * 该缓冲默认关闭；调度和 timer 热路径只在 active 非零时进入加锁记录路径。
 */
void
schedtrace_init(void)
{
  initlock(&trace.lock, "schedtrace");
  schedtrace_reset();
}

/**
 * schedtrace_reset 清空当前 session 的事件、过滤器和 overflow 计数。
 *
 * 调用者通常先创建 worker 并注册 PID，再 start；reset 不读取进程表，也不获取
 * runq.lock 或 p->lock，避免形成 trace lock -> 调度锁的反向锁序。
 */
void
schedtrace_reset(void)
{
  acquire(&trace.lock);
  trace.active = 0;
  trace.dropped = 0;
  trace.events = 0;
  trace.next_seq = 1;
  memset(trace.filters, 0, sizeof(trace.filters));
  release(&trace.lock);
}

/**
 * schedtrace_start 显式开启采样。
 *
 * @return 总是返回 0；保留返回值便于未来拒绝非法状态。
 */
int
schedtrace_start(void)
{
  acquire(&trace.lock);
  trace.active = 1;
  release(&trace.lock);
  return 0;
}

/**
 * schedtrace_stop 显式关闭采样。
 *
 * @return 总是返回 0；关闭后保留最近一次完整 session 数据供 read/dump 使用。
 */
int
schedtrace_stop(void)
{
  acquire(&trace.lock);
  trace.active = 0;
  release(&trace.lock);
  return 0;
}

/**
 * schedtrace_watch_pid 将一个现存进程加入当前 session 的 PID/generation 过滤器。
 *
 * @param pid 需要观察的 xv6 进程号；必须对应一个活跃 proc。
 * @return 新增或已存在时返回 0；pid 不存在或过滤器满时返回 -1。
 */
int
schedtrace_watch_pid(int pid)
{
  int generation = 0;

  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid && p->state != UNUSED)
      generation = p->sched.generation;
    release(&p->lock);
    if(generation)
      break;
  }
  if(generation == 0)
    return -1;

  acquire(&trace.lock);
  for(int i = 0; i < SCHEDTRACE_MAX_FILTERS; i++){
    if(trace.filters[i].pid == pid && trace.filters[i].generation == generation){
      release(&trace.lock);
      return 0;
    }
  }
  for(int i = 0; i < SCHEDTRACE_MAX_FILTERS; i++){
    if(trace.filters[i].pid == 0){
      trace.filters[i].pid = pid;
      trace.filters[i].generation = generation;
      release(&trace.lock);
      return 0;
    }
  }
  release(&trace.lock);
  return -1;
}

/**
 * filter_matches_locked 判断进程是否属于当前 trace session。
 *
 * @param p 调度器当前持有 p->lock 的进程。
 * @return 匹配任一 PID/generation 过滤器时返回 1，否则返回 0。
 */
static int
filter_matches_locked(struct proc *p)
{
  for(int i = 0; i < SCHEDTRACE_MAX_FILTERS; i++)
    if(trace.filters[i].pid == p->pid &&
       trace.filters[i].generation == p->sched.generation)
      return 1;
  return 0;
}

/**
 * append_event_locked 向固定数组追加事件；满时只增加 dropped。
 *
 * @param event 已由调用方填好的事件，函数会补充全局递增 seq。
 */
static void
append_event_locked(struct schedtrace_event *event)
{
  if(trace.events >= SCHEDTRACE_MAX_EVENTS){
    trace.dropped++;
    return;
  }
  event->seq = trace.next_seq++;
  trace.events_buffer[trace.events++] = *event;
}

/**
 * fill_proc_event 在 p->lock 保护下复制绘图需要的调度状态。
 *
 * @param event 输出事件结构体。
 * @param p 当前运行或刚停止的进程；调用方必须持有 p->lock。
 * @param type SCHEDTRACE_EVENT_RUN_START 或 SCHEDTRACE_EVENT_RUN_STOP。
 * @param reason RUN_STOP 的停止原因；RUN_START 使用 NONE。
 */
static void
fill_proc_event(struct schedtrace_event *event, struct proc *p, int type, int reason)
{
  memset(event, 0, sizeof(*event));
  event->timestamp = r_time();
  event->scheduler_clock = p->sched.last_clock;
  event->cpu_id = cpuid();
  event->pid = p->pid;
  event->event_type = type;
  event->stop_reason = reason;
  event->process_state = p->state;
  event->slice_ticks = p->sched.slice_ticks;
  event->runtime_ticks = p->sched.runtime_ticks;
  event->dispatches = p->sched.dispatches;
  event->remaining_hint = p->sched.remaining_hint;
  event->mlfq_level = p->sched.mlfq_level;
  event->mlfq_epoch = p->sched.mlfq_epoch;
  event->weight = p->sched.weight;
  event->vruntime = p->sched.vruntime;
  safestrcpy(event->process_name, p->name, sizeof(event->process_name));
}

/**
 * schedtrace_record_start 记录进程被 dispatch 到 CPU 的起点。
 *
 * 调用方持有 p->lock，函数只额外获取 trace.lock；禁止在该路径获取 runq.lock。
 */
void
schedtrace_record_start(struct proc *p)
{
  struct schedtrace_event event;

  acquire(&trace.lock);
  if(trace.active && filter_matches_locked(p)){
    fill_proc_event(&event, p, SCHEDTRACE_EVENT_RUN_START, SCHEDTRACE_REASON_NONE);
    append_event_locked(&event);
  }
  release(&trace.lock);
}

/**
 * schedtrace_record_stop 记录进程离开 CPU 的终点。
 *
 * @param p 刚从 swtch 返回 scheduler 的进程；调用方仍持有 p->lock。
 * @param reason 根据 timer pending reason 和最终进程状态归类的停止原因。
 */
void
schedtrace_record_stop(struct proc *p, int reason)
{
  struct schedtrace_event event;

  acquire(&trace.lock);
  if(trace.active && filter_matches_locked(p)){
    fill_proc_event(&event, p, SCHEDTRACE_EVENT_RUN_STOP, reason);
    append_event_locked(&event);
  }
  release(&trace.lock);
}

/**
 * schedtrace_record_boost 记录 MLFQ 全局 boost 标记。
 *
 * @param scheduler_clock 策略内部 runq.clock，解释 boost 触发位置。
 * @param epoch boost 后的新 epoch。
 */
void
schedtrace_record_boost(unsigned long scheduler_clock, unsigned long epoch)
{
  struct schedtrace_event event;

  acquire(&trace.lock);
  if(trace.active){
    memset(&event, 0, sizeof(event));
    event.timestamp = r_time();
    event.scheduler_clock = scheduler_clock;
    event.cpu_id = cpuid();
    event.pid = -1;
    event.event_type = SCHEDTRACE_EVENT_MLFQ_BOOST;
    event.stop_reason = SCHEDTRACE_REASON_NONE;
    event.mlfq_epoch = epoch;
    safestrcpy(event.process_name, "mlfq-boost", sizeof(event.process_name));
    append_event_locked(&event);
  }
  release(&trace.lock);
}

/**
 * schedtrace_copy_snapshot 将当前事件短时间复制到调用方提供的内核缓冲。
 *
 * @param snapshot 输出快照，必须位于内核地址空间。
 * @param max_events 调用者愿意接收的最大事件数，不能超过 ABI 固定上限。
 * @return 成功返回实际复制事件数；容量非法时返回 -1。
 */
int
schedtrace_copy_snapshot(struct schedtrace_snapshot *snapshot, int max_events)
{
  int n;

  if(max_events < 0 || max_events > SCHEDTRACE_MAX_EVENTS)
    return -1;

  acquire(&trace.lock);
  n = trace.events;
  if(n > max_events)
    n = max_events;

  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->version = SCHEDTRACE_VERSION;
  snapshot->policy = SCHED_POLICY;
  snapshot->cpus = XV6_CPUS;
  snapshot->events = n;
  snapshot->dropped = trace.dropped + (trace.events - n);
  snapshot->capacity = SCHEDTRACE_MAX_EVENTS;
  snapshot->active = trace.active;
  memmove(snapshot->events_buffer, trace.events_buffer,
          n * sizeof(struct schedtrace_event));
  release(&trace.lock);
  return n;
}
