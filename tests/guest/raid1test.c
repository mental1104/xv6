#include "kernel/types.h"
#include "kernel/raid1.h"
#include "user/user.h"

// 固定大小数据放在 BSS，避免占用 xv6 单页用户栈。
static uchar payload[RAID1_PAYLOAD_SIZE];
static struct raid1_info info;
static struct raid1_result result;

/**
 * 输出稳定失败原因并终止测试进程。
 *
 * @param message 不含换行的失败说明。
 */
static void
fail(char *message)
{
  printf("RAID1TEST error=%s\n", message);
  exit(1);
}

/**
 * 根据逻辑块号和种子生成可跨重启复算的测试有效载荷。
 *
 * @param blockno RAID1 逻辑块号。
 * @param seed 命令行提供的非负种子。
 */
static void
fill_payload(uint blockno, int seed)
{
  for(uint i = 0; i < RAID1_PAYLOAD_SIZE; i++)
    payload[i] = (uchar)((seed + blockno * 31U + i * 17U) & 0xffU);
}

/**
 * 验证当前缓冲区仍与指定块号和种子生成的模式完全一致。
 *
 * @param blockno RAID1 逻辑块号。
 * @param seed 写入阶段使用的种子。
 */
static void
check_payload(uint blockno, int seed)
{
  for(uint i = 0; i < RAID1_PAYLOAD_SIZE; i++){
    uchar expected = (uchar)((seed + blockno * 31U + i * 17U) & 0xffU);
    if(payload[i] != expected)
      fail("payload-mismatch");
  }
}

/**
 * 读取并验证成员掩码、最小容量和有效载荷大小。
 *
 * @param expected_mask 本轮 QEMU 拓扑中应在线的成员掩码。
 */
static void
run_info(uint expected_mask)
{
  if(raid1info(&info) < 0)
    fail("info-syscall");
  if(info.present_mask != expected_mask)
    fail("present-mask");
  if(expected_mask != 0 && info.logical_blocks == 0)
    fail("logical-capacity");
  if(info.payload_bytes != RAID1_PAYLOAD_SIZE)
    fail("payload-size");

  printf("RAID1TEST op=info present=%d logical_blocks=%d payload=%d member0=%d member1=%d\n",
         info.present_mask, (int)info.logical_blocks, info.payload_bytes,
         (int)info.member_blocks[0], (int)info.member_blocks[1]);
}

/**
 * 镜像写入确定性数据，并验证成功成员掩码。
 *
 * @param blockno RAID1 逻辑块号。
 * @param seed 有效载荷生成种子。
 * @param expected_completed 应完成写入的成员掩码。
 */
static void
run_write(uint blockno, int seed, uint expected_completed)
{
  fill_payload(blockno, seed);
  if(raid1rw(RAID1_OP_WRITE, blockno, payload, &result) < 0)
    fail("write-syscall");
  if(result.attempted_mask != expected_completed ||
     result.completed_mask != expected_completed ||
     result.repaired_mask != 0 || result.source_member != -1)
    fail("write-result");

  printf("RAID1TEST op=write block=%d attempted=%d completed=%d\n",
         blockno, result.attempted_mask, result.completed_mask);
}

/**
 * 读取确定性数据，并验证有效副本来源和读时修复结果。
 *
 * @param blockno RAID1 逻辑块号。
 * @param seed 写入阶段使用的种子。
 * @param expected_attempted 本轮应尝试读取的在线成员掩码。
 * @param expected_completed 校验通过的原始副本掩码。
 * @param expected_source 预期数据来源成员下标。
 * @param expected_repaired 预期被读时修复的成员掩码。
 */
static void
run_read(uint blockno, int seed, uint expected_attempted,
         uint expected_completed, int expected_source,
         uint expected_repaired)
{
  memset(payload, 0, sizeof(payload));
  if(raid1rw(RAID1_OP_READ, blockno, payload, &result) < 0)
    fail("read-syscall");
  check_payload(blockno, seed);
  if(result.attempted_mask != expected_attempted ||
     result.completed_mask != expected_completed ||
     result.source_member != expected_source ||
     result.repaired_mask != expected_repaired)
    fail("read-result");

  printf("RAID1TEST op=read block=%d attempted=%d valid=%d source=%d repaired=%d\n",
         blockno, result.attempted_mask, result.completed_mask,
         result.source_member, result.repaired_mask);
}

/**
 * 解析 info、write 或 read 子命令并执行一组精确断言。
 *
 * @param argc 命令行参数数量。
 * @param argv 命令和十进制参数数组。
 * @return 不返回；成功或失败都通过 exit() 结束。
 */
int
main(int argc, char **argv)
{
  if(argc == 3 && strcmp(argv[1], "info") == 0){
    run_info((uint)atoi(argv[2]));
  } else if(argc == 5 && strcmp(argv[1], "write") == 0){
    run_write((uint)atoi(argv[2]), atoi(argv[3]), (uint)atoi(argv[4]));
  } else if(argc == 8 && strcmp(argv[1], "read") == 0){
    run_read((uint)atoi(argv[2]), atoi(argv[3]), (uint)atoi(argv[4]),
             (uint)atoi(argv[5]), atoi(argv[6]), (uint)atoi(argv[7]));
  } else {
    fail("usage");
  }

  printf("RAID1TEST result=ok\n");
  exit(0);
}
