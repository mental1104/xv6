#ifndef XV6_SCHEDTRACE_ABI_H
#define XV6_SCHEDTRACE_ABI_H

#define SCHEDTRACE_VERSION 1
#define SCHEDTRACE_MAX_EVENTS 512
#define SCHEDTRACE_MAX_FILTERS 8
#define SCHEDTRACE_NAME_LEN 16

// schedtrace() 支持的控制操作；用户态通过同一个 syscall 显式启停采样。
enum schedtrace_op {
  SCHEDTRACE_OP_RESET = 1,
  SCHEDTRACE_OP_START = 2,
  SCHEDTRACE_OP_STOP = 3,
  SCHEDTRACE_OP_WATCH_PID = 4,
  SCHEDTRACE_OP_READ = 5,
};

// 调度轨迹事件类型；RUN_START/RUN_STOP 可由用户态合成为运行矩形。
enum schedtrace_event_type {
  SCHEDTRACE_EVENT_RUN_START = 1,
  SCHEDTRACE_EVENT_RUN_STOP = 2,
  SCHEDTRACE_EVENT_MLFQ_BOOST = 3,
};

// 停止原因描述进程为什么离开 CPU，而不是仅说明发生了 swtch。
enum schedtrace_stop_reason {
  SCHEDTRACE_REASON_NONE = 0,
  SCHEDTRACE_REASON_RR_QUANTUM = 1,
  SCHEDTRACE_REASON_STCF_SHORTER_TASK = 2,
  SCHEDTRACE_REASON_MLFQ_QUANTUM = 3,
  SCHEDTRACE_REASON_MLFQ_HIGHER_QUEUE = 4,
  SCHEDTRACE_REASON_CFS_LOWER_VRUNTIME = 5,
  SCHEDTRACE_REASON_VOLUNTARY_YIELD = 6,
  SCHEDTRACE_REASON_SLEEP = 7,
  SCHEDTRACE_REASON_EXIT = 8,
  SCHEDTRACE_REASON_KILLED = 9,
  SCHEDTRACE_REASON_UNKNOWN = 10,
};

// 用户可见的单条调度事件。字段顺序也是 schedviz dump 的稳定输出顺序。
struct schedtrace_event {
  unsigned long seq;              // 单调递增的事件序号，帮助用户态检查丢失和排序。
  unsigned long timestamp;        // RISC-V time CSR 时间戳，用于 SVG 横轴近似排序。
  unsigned long scheduler_clock;  // 调度器逻辑时钟，便于和策略统计对齐。
  int cpu_id;                     // 产生事件的 CPU 编号。
  int pid;                        // 被观察进程的 pid。
  int event_type;                 // schedtrace_event_type。
  int stop_reason;                // RUN_STOP 事件的 schedtrace_stop_reason。
  int process_state;              // 记录事件时的 procstate。
  unsigned long slice_ticks;      // 本次连续运行片的 tick 数。
  unsigned long runtime_ticks;    // 进程累计运行 tick。
  unsigned long dispatches;       // 进程累计被调度次数。
  unsigned long remaining_hint;   // SJF/STCF 剩余运行提示。
  int mlfq_level;                 // MLFQ 队列层级，非 MLFQ 策略也稳定输出。
  unsigned long mlfq_epoch;       // MLFQ boost 代际。
  int weight;                     // CFS 权重。
  unsigned long vruntime;         // CFS 虚拟运行时间。
  char process_name[SCHEDTRACE_NAME_LEN]; // xv6 进程名快照。
};

// schedtrace read 返回的固定头部；events[] 由调用者按 max_events 指定容量。
struct schedtrace_snapshot {
  int version;                    // ABI 版本号。
  int policy;                     // 编译期选择的调度策略编号。
  int cpus;                       // 编译期 CPUS 值，用于渲染 CPU lane。
  int events;                     // 当前快照中有效事件数量。
  int dropped;                    // 环形缓冲容量不足时丢弃的事件数量。
  int capacity;                   // 内核固定缓冲容量。
  int active;                     // 采样是否仍处于启动状态。
  int reserved;                   // 保留字段，保持结构体对齐和未来扩展空间。
  struct schedtrace_event events_buffer[SCHEDTRACE_MAX_EVENTS];
};

#endif
