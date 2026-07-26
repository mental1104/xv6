#ifndef XV6_DISKTRACE_ABI_H
#define XV6_DISKTRACE_ABI_H

#define DISKTRACE_VERSION 1
#define DISKTRACE_MAX_EVENTS 128

// disktrace() 通过同一个系统调用显式控制一次块设备驱动观察 session。
enum disktrace_op {
  DISKTRACE_OP_RESET = 1,
  DISKTRACE_OP_START = 2,
  DISKTRACE_OP_STOP = 3,
  DISKTRACE_OP_READ = 4,
};

// 事件只描述 xv6 virtio 驱动可见的边界，不推断 QEMU 设备内部调度或机械寻道。
enum disktrace_stage {
  DISKTRACE_STAGE_SUBMIT = 1,
  DISKTRACE_STAGE_QUEUED = 2,
  DISKTRACE_STAGE_COMPLETE = 3,
  DISKTRACE_STAGE_RETURN = 4,
};

/** 描述一个块请求穿过 xv6 virtio 驱动边界时的稳定事件。 */
struct disktrace_event {
  uint64 seq;          // 当前 session 内单调递增的事件序号。
  uint64 request_id;   // 驱动分配的全局请求编号，用于关联四个阶段。
  uint64 timestamp;    // RISC-V time CSR 采样值，只用于事件排序观察。
  uint64 blockno;      // struct buf 的 xv6 文件系统块号。
  int stage;           // disktrace_stage。
  int write;           // 0 为读，1 为写。
  int descriptor;      // 三描述符链的首描述符下标。
  int queue_index;     // 事件发生时的 avail/used 环形队列位置快照。
};

/** disktrace read 返回的固定头部与事件数组。 */
struct disktrace_snapshot {
  int version;
  int events;
  int dropped;
  int capacity;
  int active;
  int reserved;
  struct disktrace_event events_buffer[DISKTRACE_MAX_EVENTS];
};

#endif
