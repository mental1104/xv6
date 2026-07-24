#ifndef XV6_KERNEL_PROCINFO_H
#define XV6_KERNEL_PROCINFO_H

/** 用户态可见的进程名称容量，与 struct proc::name 保持一致。 */
#define PROCINFO_NAME_LENGTH 16

/**
 * 定义进程快照使用的稳定公开状态。
 *
 * 该枚举不直接复用内核 enum procstate 的数值，避免内核状态布局调整时破坏
 * 用户程序 ABI。UNKNOWN 为未来新增但尚未映射的内核状态保留。
 */
enum procinfo_state {
  PROCINFO_STATE_UNKNOWN = 0,
  PROCINFO_STATE_USED,
  PROCINFO_STATE_SLEEPING,
  PROCINFO_STATE_RUNNABLE,
  PROCINFO_STATE_RUNNING,
  PROCINFO_STATE_STOPPED,
  PROCINFO_STATE_ZOMBIE,
};

/**
 * 描述 getprocs() 返回的单个进程快照。
 *
 * 每个结构体在目标进程锁保护下形成一致副本；整张进程表不是全局原子快照。
 */
struct procinfo {
  int pid;
  int ppid;
  int state;
  char name[PROCINFO_NAME_LENGTH];
};

#endif
