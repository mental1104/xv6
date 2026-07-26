#ifndef XV6_KERNEL_RAID1_H
#define XV6_KERNEL_RAID1_H

#define RAID1_MEMBER_COUNT 2
#define RAID1_PAYLOAD_SIZE 1008

#define RAID1_OP_READ 0
#define RAID1_OP_WRITE 1

/** 描述 RAID1 教学层当前可见的成员与逻辑容量。 */
struct raid1_info {
  uint present_mask;
  uint payload_bytes;
  uint64 member_blocks[RAID1_MEMBER_COUNT];
  uint64 logical_blocks;
};

/** 描述一次 RAID1 读写实际触达、完成和修复的成员。 */
struct raid1_result {
  uint attempted_mask;
  uint completed_mask;
  uint repaired_mask;
  int source_member;
};

#endif
