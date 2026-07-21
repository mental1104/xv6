#include "rbtree.h"

struct proc;

// Saved registers for kernel context switches.
struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// Per-CPU state.
struct cpu {
  struct proc *proc;          // The process running on this cpu, or null.
  struct context context;     // swtch() here to enter scheduler().
  int noff;                   // Depth of push_off() nesting.
  int intena;                 // Were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];

// per-process data for the trap handling code in trampoline.S.
// sits in a page by itself just under the trampoline page in the
// user page table. not specially mapped in the kernel page table.
// the sscratch register points here.
// uservec in trampoline.S saves user registers in the trapframe,
// then initializes registers from the trapframe's
// kernel_sp, kernel_hartid, kernel_satp, and jumps to kernel_trap.
// usertrapret() and userret in trampoline.S set up
// the trapframe's kernel_*, restore user registers, switch to the user
// page table, and enter user space.
struct trapframe {
  /*   0 */ uint64 kernel_satp;
  /*   8 */ uint64 kernel_sp;
  /*  16 */ uint64 kernel_trap;
  /*  24 */ uint64 epc;
  /*  32 */ uint64 kernel_hartid;
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

struct user_context {
  uint64 epc;
  uint64 gpr[31];
};

enum procstate { UNUSED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Scheduling metadata is embedded in proc so queue operations never allocate
// memory from interrupt or scheduler context.
struct sched_entity {
  struct proc *prev;
  struct proc *next;
  struct rb_node rb;
  int on_rq;
  int reserved;
  int generation;
  int weight;
  int mlfq_level;
  int force_preempt;
  uint64 mlfq_used;
  uint64 mlfq_epoch;
  uint64 enqueue_seq;
  uint64 runtime_ticks;
  uint64 dispatches;
  uint64 burst_hint;
  uint64 remaining_hint;
  uint64 vruntime;
  uint64 slice_ticks;
};

// Per-process state
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state;
  struct proc *parent;
  void *chan;
  int killed;
  int xstate;
  int pid;
  struct sched_entity sched;

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;
  uint64 sz;
  pagetable_t pagetable;
  pagetable_t kpagetable;
  struct trapframe *trapframe;
  struct context context;
  struct file *ofile[NOFILE];
  struct inode *cwd;
  char name[16];
  int mask;
  void (*handler)();
  int alarm_interval;
  int total_ticks;
  uint64 in_handler;
  struct user_context alarm_context;
  struct VMA* vma[NOFILE];
};
