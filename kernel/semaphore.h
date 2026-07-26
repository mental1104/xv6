#ifndef XV6_KERNEL_SEMAPHORE_H
#define XV6_KERNEL_SEMAPHORE_H

/**
 * 用户态可读取的教学型信号量状态快照。
 *
 * owner_pid 只表示负责销毁和退出清理的生命周期所有者，不表示许可证只能由
 * 创建者消费或归还；持有有效句柄的进程都可以执行 wait/post。
 */
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
