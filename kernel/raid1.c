#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "defs.h"
#include "raid1.h"

#define RAID1_MAGIC 0x52414944U
#define RAID1_ROOT_DEVICE_COUNT 1

/** RAID1 成员盘中一个物理块的稳定教学格式。 */
struct raid1_record {
  uint magic;
  uint blockno;
  uint payload_bytes;
  uint checksum;
  uchar payload[RAID1_PAYLOAD_SIZE];
};

typedef char raid1_record_must_fill_one_block[
  sizeof(struct raid1_record) == BSIZE ? 1 : -1
];

static struct {
  struct sleeplock lock;
  uint present_mask;
  uint64 member_blocks[RAID1_MEMBER_COUNT];
  uint64 logical_blocks;
  struct buf buffers[RAID1_MEMBER_COUNT];
} raid1;

/**
 * 将 RAID1 成员下标转换为底层 virtio 块设备编号。
 *
 * @param member RAID1 成员下标，范围为 0 到 RAID1_MEMBER_COUNT-1。
 * @return 底层设备编号；0 保留给 xv6 根文件系统。
 */
static int
raid1_device(int member)
{
  return RAID1_ROOT_DEVICE_COUNT + member;
}

/**
 * 计算一段有效载荷的 FNV-1a 校验值。
 *
 * @param data 待校验数据，调用期间只读。
 * @param length 数据字节数。
 * @return 32 位校验值。
 */
static uint
raid1_checksum(const uchar *data, uint length)
{
  uint hash = 2166136261U;

  for(uint i = 0; i < length; i++){
    hash ^= data[i];
    hash *= 16777619U;
  }
  return hash;
}

/**
 * 判断成员盘读出的物理块是否属于请求的逻辑块且内容完整。
 *
 * @param record 待验证的磁盘记录。
 * @param blockno 调用者请求的 RAID1 逻辑块号。
 * @return 元数据和校验值全部匹配时返回 1，否则返回 0。
 */
static int
raid1_record_valid(const struct raid1_record *record, uint blockno)
{
  if(record->magic != RAID1_MAGIC ||
     record->blockno != blockno ||
     record->payload_bytes != RAID1_PAYLOAD_SIZE)
    return 0;

  return record->checksum ==
         raid1_checksum(record->payload, RAID1_PAYLOAD_SIZE);
}

/**
 * 对一个已探测到的 RAID1 成员执行整块读写。
 *
 * @param member RAID1 成员下标。
 * @param blockno 成员盘物理块号，与 RAID1 逻辑块号一一对应。
 * @param write 非零表示写入，零表示读取。
 * @return virtio 请求成功返回 0；设备拒绝或未完成时返回 -1。
 */
static int
raid1_member_rw(int member, uint blockno, int write)
{
  struct buf *buffer = &raid1.buffers[member];

  buffer->blockno = blockno;
  buffer->disk = 0;
  return virtio_disk_rw_device(raid1_device(member), buffer, write);
}

/**
 * 初始化两个可选 virtio 成员的存在性和可用逻辑容量。
 *
 * 根文件系统仍独占 virtio 设备 0；成员盘缺失不会阻止 xv6 启动。
 */
void
raid1_init(void)
{
  initsleeplock(&raid1.lock, "raid1");
  raid1.present_mask = 0;
  raid1.logical_blocks = 0;

  for(int member = 0; member < RAID1_MEMBER_COUNT; member++){
    int device = raid1_device(member);
    raid1.member_blocks[member] = 0;
    if(!virtio_disk_present(device))
      continue;

    raid1.present_mask |= 1U << member;
    raid1.member_blocks[member] = virtio_disk_capacity(device);
    if(raid1.logical_blocks == 0 ||
       raid1.member_blocks[member] < raid1.logical_blocks)
      raid1.logical_blocks = raid1.member_blocks[member];
  }
}

/**
 * 复制 RAID1 教学层当前成员和容量信息。
 *
 * @param info 接收快照的内核缓冲区，调用者持有。
 * @return 始终返回 0；成员全部缺失通过 present_mask=0 表达。
 */
int
raid1_get_info(struct raid1_info *info)
{
  info->present_mask = raid1.present_mask;
  info->payload_bytes = RAID1_PAYLOAD_SIZE;
  info->logical_blocks = raid1.logical_blocks;
  for(int member = 0; member < RAID1_MEMBER_COUNT; member++)
    info->member_blocks[member] = raid1.member_blocks[member];
  return 0;
}

/**
 * 将一个固定大小有效载荷镜像写入所有在线成员。
 *
 * @param blockno RAID1 逻辑块号。
 * @param payload 必须包含 RAID1_PAYLOAD_SIZE 字节；调用期间只读。
 * @param result 接收尝试和成功成员掩码，调用者持有。
 * @return 至少一个成员写入成功时返回 0；越界或全部写入失败时返回 -1。
 */
static int
raid1_write(uint blockno, const uchar *payload, struct raid1_result *result)
{
  if(raid1.present_mask == 0 || blockno >= raid1.logical_blocks)
    return -1;

  for(int member = 0; member < RAID1_MEMBER_COUNT; member++){
    uint bit = 1U << member;
    if((raid1.present_mask & bit) == 0)
      continue;

    struct raid1_record *record =
      (struct raid1_record *)raid1.buffers[member].data;
    record->magic = RAID1_MAGIC;
    record->blockno = blockno;
    record->payload_bytes = RAID1_PAYLOAD_SIZE;
    memmove(record->payload, payload, RAID1_PAYLOAD_SIZE);
    record->checksum = raid1_checksum(record->payload, RAID1_PAYLOAD_SIZE);

    result->attempted_mask |= bit;
    if(raid1_member_rw(member, blockno, 1) == 0)
      result->completed_mask |= bit;
  }

  return result->completed_mask != 0 ? 0 : -1;
}

/**
 * 从镜像成员读取一个有效载荷，并在只有一份有效副本时修复在线坏副本。
 *
 * @param blockno RAID1 逻辑块号。
 * @param payload 接收 RAID1_PAYLOAD_SIZE 字节的调用者缓冲区。
 * @param result 接收读取、有效副本、来源和修复成员信息。
 * @return 找到一致有效副本时返回 0；越界、无有效副本或双副本分歧时返回 -1。
 */
static int
raid1_read(uint blockno, uchar *payload, struct raid1_result *result)
{
  if(raid1.present_mask == 0 || blockno >= raid1.logical_blocks)
    return -1;

  for(int member = 0; member < RAID1_MEMBER_COUNT; member++){
    uint bit = 1U << member;
    if((raid1.present_mask & bit) == 0)
      continue;

    result->attempted_mask |= bit;
    if(raid1_member_rw(member, blockno, 0) < 0)
      continue;

    struct raid1_record *record =
      (struct raid1_record *)raid1.buffers[member].data;
    if(raid1_record_valid(record, blockno))
      result->completed_mask |= bit;
  }

  if(result->completed_mask == 0)
    return -1;

  int source = (result->completed_mask & 1U) ? 0 : 1;
  result->source_member = source;
  struct raid1_record *source_record =
    (struct raid1_record *)raid1.buffers[source].data;

  // 两份校验都正确却内容不同，两个副本不足以投票决定哪份更新，必须拒绝静默选择。
  if(result->completed_mask == 3U){
    struct raid1_record *other =
      (struct raid1_record *)raid1.buffers[1].data;
    if(memcmp(source_record->payload, other->payload,
              RAID1_PAYLOAD_SIZE) != 0)
      return -1;
  }

  memmove(payload, source_record->payload, RAID1_PAYLOAD_SIZE);

  // 仅修复在线且未通过校验的成员；缺失成员不被伪装成已恢复。
  for(int member = 0; member < RAID1_MEMBER_COUNT; member++){
    uint bit = 1U << member;
    if((raid1.present_mask & bit) == 0 ||
       (result->completed_mask & bit) != 0)
      continue;

    memmove(raid1.buffers[member].data, source_record, BSIZE);
    if(raid1_member_rw(member, blockno, 1) == 0)
      result->repaired_mask |= bit;
  }

  return 0;
}

/**
 * 在统一锁内执行一次 RAID1 教学读写，保护共享块缓冲区和修复顺序。
 *
 * @param write RAID1_OP_WRITE 表示写入，RAID1_OP_READ 表示读取。
 * @param blockno RAID1 逻辑块号。
 * @param payload 固定大小输入或输出缓冲区，所有权不转移。
 * @param result 接收本次操作可观察结果，所有字段由本函数初始化。
 * @return 操作成功返回 0；参数、容量、一致性或底层 I/O 失败返回 -1。
 */
int
raid1_rw(int write, uint blockno, uchar *payload, struct raid1_result *result)
{
  result->attempted_mask = 0;
  result->completed_mask = 0;
  result->repaired_mask = 0;
  result->source_member = -1;

  if(write != RAID1_OP_READ && write != RAID1_OP_WRITE)
    return -1;

  acquiresleep(&raid1.lock);
  int status;
  if(write == RAID1_OP_WRITE)
    status = raid1_write(blockno, payload, result);
  else
    status = raid1_read(blockno, payload, result);
  releasesleep(&raid1.lock);
  return status;
}
