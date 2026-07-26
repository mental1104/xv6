#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "raid1.h"

/**
 * 将 RAID1 成员与容量快照复制到用户空间。
 *
 * @return 成功返回 0；用户地址不可写时返回 -1。
 */
uint64
sys_raid1info(void)
{
  uint64 user_info;
  struct raid1_info info;

  argaddr(0, &user_info);
  if(raid1_get_info(&info) < 0)
    return -1;
  if(copyout(myproc()->pagetable, user_info, (char *)&info,
             sizeof(info)) < 0)
    return -1;
  return 0;
}

/**
 * 在用户缓冲区与 RAID1 教学层之间传输一个固定大小逻辑块。
 *
 * @return 读写和结果复制全部成功时返回 0；参数、用户地址或 RAID1 操作失败时返回 -1。
 */
uint64
sys_raid1rw(void)
{
  int operation;
  int blockno;
  uint64 user_payload;
  uint64 user_result;
  uchar payload[RAID1_PAYLOAD_SIZE];
  struct raid1_result result;

  argint(0, &operation);
  argint(1, &blockno);
  argaddr(2, &user_payload);
  argaddr(3, &user_result);
  if(blockno < 0)
    return -1;

  if(operation == RAID1_OP_WRITE &&
     copyin(myproc()->pagetable, (char *)payload, user_payload,
            sizeof(payload)) < 0)
    return -1;

  int status = raid1_rw(operation, (uint)blockno, payload, &result);

  // 即使 RAID1 拒绝操作，也先返回已尝试成员，便于实验区分越界、无副本和一致性失败。
  if(copyout(myproc()->pagetable, user_result, (char *)&result,
             sizeof(result)) < 0)
    return -1;
  if(status < 0)
    return -1;

  if(operation == RAID1_OP_READ &&
     copyout(myproc()->pagetable, user_payload, (char *)payload,
             sizeof(payload)) < 0)
    return -1;
  return 0;
}
