#ifndef XV6_KERNEL_CONCURRENCYLAB_H
#define XV6_KERNEL_CONCURRENCYLAB_H

#define CONCURRENCYLAB_PARTICIPANTS 2

#define CONCURRENCYLAB_MODE_RACY   1
#define CONCURRENCYLAB_MODE_LOCKED 2

#define CONCURRENCYLAB_OP_RESET    1
#define CONCURRENCYLAB_OP_RUN      2
#define CONCURRENCYLAB_OP_SNAPSHOT 3
#define CONCURRENCYLAB_OP_CLOSE    4

/** 描述一个实验参与者对共享计数器执行读—改—写时留下的结构化轨迹。 */
struct concurrencylab_worker_snapshot {
  int role;
  int observed;
  int written;
  int read_order;
  int write_order;
  int read_cpu;
  int write_cpu;
};

/** 描述一次两参与者并发实验完成后的共享状态与顺序证据。 */
struct concurrencylab_snapshot {
  int session;
  int mode;
  int configured_cpus;
  int active;
  int completed;
  int counter;
  struct concurrencylab_worker_snapshot workers[CONCURRENCYLAB_PARTICIPANTS];
};

#endif
