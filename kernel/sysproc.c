#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

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

uint64
sys_sbrk(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;

  struct proc *p = myproc();
  uint64 oldsz = p->sz;
  if(n < 0){
    uint64 shrink = (uint64)(-n);
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
    if(newsz < oldsz || PGROUNDUP(newsz) >= PLIC)
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
