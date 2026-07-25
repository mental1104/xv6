#ifndef XV6_KERNEL_SEMAPHORE_H
#define XV6_KERNEL_SEMAPHORE_H

#define SEMAPHORE_MAX_COUNT 0x7fffffff

/** 用户态可读取的教学型信号量状态快照。 */
struct semaphore_info {
  int handle;
  int owner_pid;
  int value;
  int limit;
  int waiters;
  uint successful_waits;
  uint posts;
  uint wake_calls;
};

#endif
