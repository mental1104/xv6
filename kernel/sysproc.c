#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"
#include "sched.h"
#include "schedstat.h"
#include "schedtrace.h"
#include "jobctl.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_waitpid(void)
{
  int pid, options;
  uint64 status;

  if(argint(0, &pid) < 0 ||
     argaddr(1, &status) < 0 ||
     argint(2, &options) < 0)
    return -1;
  return waitpid(pid, status, options);
}

/** 将当前进程或直接子进程放入指定进程组。 */
uint64
sys_setpgid(void)
{
  int pid;
  int pgid;

  if(argint(0, &pid) < 0 || argint(1, &pgid) < 0)
    return -1;
  return setpgid(pid, pgid);
}

/** 查询当前进程或直接子进程的 PGID。 */
uint64
sys_getpgid(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return getpgid(pid);
}

/** 对整个进程组执行停止、继续或终止动作。 */
uint64
sys_procctl(void)
{
  int pgid;
  int action;

  if(argint(0, &pgid) < 0 || argint(1, &action) < 0)
    return -1;
  if(action != JOBCTL_STOP && action != JOBCTL_CONT && action != JOBCTL_TERM)
    return -1;
  return proc_group_control(pgid, action);
}

uint64
sys_sbrk(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;

  struct proc *p = myproc();
  uint64 oldsz = p->sz;
  if(n < 0){
    // 先提升到 int64 再取反，避免 n == INT_MIN 时发生有符号整数溢出。
    uint64 shrink = (uint64)(-(int64)n);
    if(shrink > oldsz)
      return -1;
    uint64 newsz = oldsz - shrink;
    if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
      uint64 start = PGROUNDUP(newsz);
      uint64 npages = (PGROUNDUP(oldsz) - start) / PGSIZE;
      u2kvmunmap(p->kpagetable, start, npages);
    }
    p->sz = uvmdealloc(p->pagetable, oldsz, newsz);
  } else {
    uint64 newsz = oldsz + (uint64)n;
    // USERMAX 是用户 VA 的一过上限；等于 USERMAX 表示最后一页恰好结束在边界。
    if(newsz < oldsz || newsz > USERMAX)
      return -1;
    p->sz = newsz;
  }
  return oldsz;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_trace(void)
{
  uint64 mask;
  if(argaddr(0, &mask) < 0)
    return -1;

  myproc()->mask = mask;
  return 0;
}

uint64
sys_sysinfo(void)
{
  uint64 addr;
  if(argaddr(0,&addr) < 0)
    return -1;

  struct sysinfo info;
  struct proc* p = myproc();

  info.freemem = free_mem();
  info.nproc = free_proc();

  if(copyout(p->pagetable, addr, (char *)&info, sizeof(info)) < 0)
      return -1;
  return 0;
}

uint64
sys_sigalarm(void)
{
    struct proc* p = myproc();
    int n;
    uint64 handler;
    if(argint(0,&n) < 0)
        return -1;
    if(argaddr(1, &handler) < 0)
        return -1;
    p->handler = (void (*)())handler;
    p->alarm_interval = n;
    return 0;
}

uint64
sys_sigreturn(void)
{
    struct proc* p = myproc();
    restore_user_context(p->trapframe, &p->alarm_context);
    p->in_handler = 0;
    return p->trapframe->a0;
}

/**
 * 保存当前用户现场，并可在同一次系统调用中恢复另一个完整用户现场。
 *
 * alarm handler 调用时，保存源取自 p->alarm_context，确保得到真正被 timer
 * 中断的 epc 和全部通用寄存器，而不是 handler 自身的系统调用现场。普通用户
 * 代码调用时，保存源取自 trapframe，并把保存的 a0 规范为 0，使该线程未来
 * 恢复后把 ucontext_switch() 视为成功返回。
 *
 * @return 仅保存时返回 0；切换时返回目标上下文原有的 a0；用户地址无效时
 *         返回 -1，且不会恢复目标上下文。
 */
uint64
sys_ucontext_switch(void)
{
  uint64 save_addr;
  uint64 restore_addr;
  uint64 guard_addr;
  struct proc *p = myproc();
  struct user_context current_context;
  struct user_context next_context;
  int zero = 0;

  if(argaddr(0, &save_addr) < 0 ||
     argaddr(1, &restore_addr) < 0 ||
     argaddr(2, &guard_addr) < 0)
    return -1;

  // 先把目标上下文复制到内核栈，允许 save 与 restore 指向同一用户缓冲区。
  if(restore_addr != 0 &&
     copyin(p->pagetable, (char *)&next_context, restore_addr,
            sizeof(next_context)) < 0)
    return -1;

  if(p->in_handler){
    memmove(&current_context, &p->alarm_context, sizeof(current_context));
  } else {
    save_user_context(&current_context, p->trapframe);
    current_context.gpr[USER_CONTEXT_A0_INDEX] = 0;
  }

  if(save_addr != 0 &&
     copyout(p->pagetable, save_addr, (char *)&current_context,
             sizeof(current_context)) < 0)
    return -1;

  // 用户调度器在修改线程状态前把 guard 置一；内核在返回目标上下文前清零，
  // 使“状态提交 + 上下文恢复”之间不存在可被 alarm 再次抢占的用户态窗口。
  if(guard_addr != 0 &&
     copyout(p->pagetable, guard_addr, (char *)&zero, sizeof(zero)) < 0)
    return -1;

  if(restore_addr == 0)
    return 0;

  restore_user_context(p->trapframe, &next_context);
  if(p->in_handler)
    p->in_handler = 0;
  return p->trapframe->a0;
}

/**
 * 设置当前进程供 SJF/STCF 教学策略使用的 CPU burst hint。
 *
 * @return 参数合法时返回 0，否则返回 -1。
 */
uint64
sys_sched_set_hint(void)
{
  int ticks_hint;
  if(argint(0, &ticks_hint) < 0)
    return -1;
  return sched_set_hint(ticks_hint);
}

/**
 * 设置当前进程供 Minimal CFS 使用的整数权重。
 *
 * @return 参数合法时返回 0，否则返回 -1。
 */
uint64
sys_sched_set_weight(void)
{
  int weight;
  if(argint(0, &weight) < 0)
    return -1;
  return sched_set_weight(weight);
}

/**
 * 将当前进程的调度统计复制到用户地址。
 *
 * @return 复制成功时返回 0，地址或状态无效时返回 -1。
 */
uint64
sys_sched_get_stats(void)
{
  uint64 addr;
  struct sched_stats stats;
  struct proc *p = myproc();

  if(argaddr(0, &addr) < 0)
    return -1;
  if(sched_get_stats(&stats) < 0)
    return -1;
  if(copyout(p->pagetable, addr, (char *)&stats, sizeof(stats)) < 0)
    return -1;
  return 0;
}

static struct spinlock schedtrace_sys_lock;
static int schedtrace_sys_lock_ready;
static struct schedtrace_snapshot schedtrace_sys_snapshot;

/**
 * ensure_schedtrace_sys_lock 初始化 schedtrace read 使用的静态快照锁。
 *
 * schedtrace_snapshot 大于一页，不能放在 xv6 较小的内核栈上；该锁只保护
 * sys_schedtrace 内部复用缓冲，不参与调度器锁序。
 */
static void
ensure_schedtrace_sys_lock(void)
{
  if(!schedtrace_sys_lock_ready){
    initlock(&schedtrace_sys_lock, "schedtracesys");
    schedtrace_sys_lock_ready = 1;
  }
}

/**
 * sys_schedtrace 控制和读取固定容量调度轨迹。
 *
 * 参数 0 是 SCHEDTRACE_OP_*；READ 时参数 1 为用户态
 * struct schedtrace_snapshot 地址、参数 2 为调用者事件容量。WATCH_PID 时参数
 * 2 是要加入过滤器的 PID。RESET/START/STOP 忽略后两个参数。
 *
 * @return 成功返回 0；未知操作、容量越界、PID 无效或 copyout 失败时返回 -1。
 */
uint64
sys_schedtrace(void)
{
  int op;
  int arg;
  uint64 address;
  struct proc *p = myproc();

  if(argint(0, &op) < 0)
    return -1;
  if(argaddr(1, &address) < 0)
    return -1;
  if(argint(2, &arg) < 0)
    return -1;

  switch(op){
  case SCHEDTRACE_OP_RESET:
    schedtrace_reset();
    return 0;
  case SCHEDTRACE_OP_START:
    return schedtrace_start();
  case SCHEDTRACE_OP_STOP:
    return schedtrace_stop();
  case SCHEDTRACE_OP_WATCH_PID:
    return schedtrace_watch_pid(arg);
  case SCHEDTRACE_OP_READ:
    ensure_schedtrace_sys_lock();
    acquire(&schedtrace_sys_lock);
    if(schedtrace_copy_snapshot(&schedtrace_sys_snapshot, arg) < 0){
      release(&schedtrace_sys_lock);
      return -1;
    }
    if(copyout(p->pagetable, address, (char *)&schedtrace_sys_snapshot,
               sizeof(schedtrace_sys_snapshot)) < 0){
      release(&schedtrace_sys_lock);
      return -1;
    }
    release(&schedtrace_sys_lock);
    return 0;
  default:
    return -1;
  }
}

/**
 * 显式打印当前系统调用路径的内核栈回溯。
 *
 * 该入口只用于教学和调试，不接受用户地址，也不修改进程状态。将回溯从
 * sys_sleep() 分离，避免普通 sleep 调用污染控制台。
 *
 * @return 打印完成后返回 0。
 */
uint64
sys_backtrace(void)
{
  backtrace();
  return 0;
}
