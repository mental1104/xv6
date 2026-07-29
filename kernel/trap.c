#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "file.h"
#include "vma.h"
#include "fcntl.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

#define SCAUSE_INTERRUPT_FLAG (1ULL << 63)
#define SCAUSE_CODE_MASK (~SCAUSE_INTERRUPT_FLAG)
#define SIP_SSIP (1L << 1)

/** 当前 xv6 显式处理的同步异常原因码。 */
enum scause_exception_code {
  SCAUSE_USER_ECALL = 8,
  SCAUSE_LOAD_PAGE_FAULT = 13,
  SCAUSE_STORE_PAGE_FAULT = 15,
};

/**
 * 当前 xv6 实际识别的 Supervisor interrupt 原因码。
 *
 * 枚举值只对应 `scause` 的低位原因码；最高位 interrupt 标志由
 * `SCAUSE_INTERRUPT_FLAG` 单独解析。
 */
enum scause_interrupt_code {
  SCAUSE_SUPERVISOR_SOFTWARE_INTERRUPT = 1,
  SCAUSE_SUPERVISOR_EXTERNAL_INTERRUPT = 9,
};

/** `devintr()` 对调用者返回的设备中断分类。 */
enum devintr_result {
  DEVINTR_NONE = 0,
  DEVINTR_DEVICE = 1,
  DEVINTR_TIMER = 2,
};

static enum devintr_result devintr(uint64 scause);

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

void
save_user_context(struct user_context *context,
                  const struct trapframe *trapframe)
{
  context->epc = trapframe->epc;
  memmove(context->gpr, &trapframe->ra, sizeof(context->gpr));
}

void
restore_user_context(struct trapframe *trapframe,
                     const struct user_context *context)
{
  trapframe->epc = context->epc;
  memmove(&trapframe->ra, context->gpr, sizeof(context->gpr));
}

static int
mmap_fault(struct proc *p, struct VMA *v, uint64 va)
{
  uint64 va0 = PGROUNDDOWN(va);
  pte_t *existing = walk(p->pagetable, va0, 0);
  if(existing && (*existing & PTE_V))
    return -1;

  char *mem = kalloc();
  if(mem == 0)
    return -1;
  memset(mem, 0, PGSIZE);

  uint64 within = va0 - v->addr;
  uint n = PGSIZE;
  if(within + n > v->length)
    n = v->length - within;

  ilock(v->file->ip);
  int readn = readi(v->file->ip, 0, (uint64)mem, v->offset + within, n);
  iunlock(v->file->ip);
  if(readn < 0){
    kfree(mem);
    return -1;
  }

  int perm = PTE_U;
  if(v->prot & PROT_READ)
    perm |= PTE_R;
  if(v->prot & PROT_WRITE)
    perm |= PTE_W;
  if(v->prot & PROT_EXEC)
    perm |= PTE_X;

  if(mappages(p->pagetable, va0, PGSIZE, (uint64)mem, perm) < 0){
    kfree(mem);
    return -1;
  }
  if(u2kvmcopy(p->pagetable, p->kpagetable, va0, va0 + PGSIZE) < 0){
    // 文件页已进入用户页表，但 alias 的中间页表也可能在 OOM 时失败。
    // 撤销用户叶子并释放物理页，让缺页处理按普通分配失败终止进程。
    uvmunmap(p->pagetable, va0, 1, 1);
    return -1;
  }
  return 0;
}

/**
 * 按 COW、mmap、lazy allocation 的既定优先级处理用户页错误。
 *
 * @param p 当前触发异常的进程；函数读取并更新它的用户页表。
 * @param scause RISC-V 页错误原因，只接受 load 或 store page fault。
 * @param va 触发异常的用户虚拟地址，允许位于页面内部。
 * @return 成功完成页物化或 COW 时返回 0；地址非法或资源分配失败时返回 -1。
 *
 * 一旦 `vma_find()` 命中，fault 已被归类为文件映射页，必须直接传播
 * `mmap_fault()` 的结果。即使物化失败也不能继续尝试匿名 lazy allocation，
 * 否则既会重复扫描 VMA，也会模糊失败路径的责任边界。
 */
static int
handle_user_page_fault(struct proc *p, uint64 scause, uint64 va)
{
  if(scause == SCAUSE_STORE_PAGE_FAULT && cow_alloc(p->pagetable, va) == 0)
    return 0;

  struct VMA *v = vma_find(p, va);
  if(v)
    return mmap_fault(p, v, va);
  return uvmlazyalloc(p, va);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  enum devintr_result which_dev = DEVINTR_NONE;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  uint64 scause = r_scause();

  // save user program counter.
  p->trapframe->epc = r_sepc();

  switch(scause){
  case SCAUSE_USER_ECALL:
    if(p->killed)
      exit(-1);
    p->trapframe->epc += 4;
    intr_on();
    syscall();
    break;
  case SCAUSE_LOAD_PAGE_FAULT:
  case SCAUSE_STORE_PAGE_FAULT:
    if(handle_user_page_fault(p, scause, r_stval()) < 0)
      p->killed = 1;
    break;
  default:
    which_dev = devintr(scause);
    if(which_dev == DEVINTR_NONE){
      printf("usertrap(): unexpected scause %p pid=%d\n", scause, p->pid);
      printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
      p->killed = 1;
    }
    break;
  }

  // consoleintr() 只记录 Ctrl-C/Ctrl-Z；在不持有 cons.lock 时应用到整个 PGID。
  console_apply_pending_control();

  if(p->killed)
    exit(-1);

  // 远端 CPU 对 RUNNING 成员只能设置 stop_requested；当前进程在这里安全停下。
  proc_stop_if_requested();

  // STOPPED 作业可能由 TERM 唤醒；恢复后必须在返回用户态前完成退出。
  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == DEVINTR_TIMER){
    p->total_ticks++;
    if(p->alarm_interval > 0 && p->total_ticks == p->alarm_interval){
      p->total_ticks = 0;
      if(p->in_handler == 0){
        save_user_context(&p->alarm_context, p->trapframe);
        p->trapframe->epc = (uint64)p->handler;
        p->in_handler = 1;
      }
    }
    yield();
  }

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that the trampoline.S's sret will use
  // to get to user space.

  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void
kerneltrap()
{
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  enum devintr_result which_dev;

  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  which_dev = devintr(scause);
  if(which_dev == DEVINTR_NONE){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // 前台作业可能正在 sleep()，因此 UART 控制事件也必须能由内核态 trap 代为执行。
  console_apply_pending_control();

  // give up the CPU if this is a timer interrupt.
  if(which_dev == DEVINTR_TIMER && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

/**
 * 识别并处理当前 xv6 支持的 Supervisor 设备中断。
 *
 * @param scause 本次 trap 的完整 `scause` 值，包含最高位 interrupt 标志。
 * @return `DEVINTR_DEVICE` 表示 PLIC 外部设备中断，`DEVINTR_TIMER` 表示
 * Machine Timer 转发的 Supervisor software interrupt；其他情况返回
 * `DEVINTR_NONE`，由调用者继续按异常或未知 trap 处理。
 */
static enum devintr_result
devintr(uint64 scause)
{
  if((scause & SCAUSE_INTERRUPT_FLAG) == 0)
    return DEVINTR_NONE;

  switch(scause & SCAUSE_CODE_MASK){
  case SCAUSE_SUPERVISOR_EXTERNAL_INTERRUPT: {
    // irq indicates which device interrupted.
    int irq = plic_claim();

    switch(irq){
    case UART0_IRQ:
      uartintr();
      break;
    case VIRTIO0_IRQ:
    case VIRTIO1_IRQ:
    case VIRTIO2_IRQ:
      // QEMU 的三个连续 virtio-mmio 插槽分别使用 IRQ 1、2、3。
      virtio_disk_intr(irq - VIRTIO0_IRQ);
      break;
    case 0:
      break;
    default:
      printf("unexpected interrupt irq=%d\n", irq);
      break;
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the device is now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return DEVINTR_DEVICE;
  }
  case SCAUSE_SUPERVISOR_SOFTWARE_INTERRUPT:
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.
    if(cpuid() == 0)
      clockintr();

    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~SIP_SSIP);
    return DEVINTR_TIMER;
  default:
    return DEVINTR_NONE;
  }
}
