#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/disksched_model.h"

/** 输出命令行用法和模型边界。 */
static void
usage(void)
{
  fprintf(2, "Usage: disksched <fcfs|sstf|scan> <start> <max-block> <request>...\n");
  fprintf(2, "The cost is logical block distance, not QEMU or physical seek time.\n");
}

/**
 * parse_uint 将十进制参数解析为 uint。
 *
 * @param text 非空十进制字符串。
 * @param value 接收解析值。
 * @return 合法且不溢出 32 位时返回 0，否则返回 -1。
 */
static int
parse_uint(char *text, uint *value)
{
  uint64 parsed = 0;

  if(text == 0 || text[0] == 0)
    return -1;
  for(char *cursor = text; *cursor != 0; cursor++){
    uint digit;

    if(*cursor < '0' || *cursor > '9')
      return -1;
    digit = *cursor - '0';
    if(parsed > (0xffffffffULL - digit) / 10)
      return -1;
    parsed = parsed * 10 + digit;
  }
  *value = (uint)parsed;
  return 0;
}

/** 返回命令行策略名称对应的模型编号。 */
static int
parse_policy(char *name)
{
  if(strcmp(name, "fcfs") == 0)
    return DISKSCHED_POLICY_FCFS;
  if(strcmp(name, "sstf") == 0)
    return DISKSCHED_POLICY_SSTF;
  if(strcmp(name, "scan") == 0)
    return DISKSCHED_POLICY_SCAN;
  return -1;
}

/** 打印模型选择出的请求顺序。 */
static void
print_order(struct disksched_result *result)
{
  for(int i = 0; i < result->count; i++){
    if(i != 0)
      printf(",");
    printf("%d", result->order[i]);
  }
}

/**
 * main 执行一个显式策略的纯用户态磁盘调度模型。
 *
 * 该命令只用于理解 FCFS/SSTF/SCAN 如何重排抽象请求，不改变 xv6 virtio 驱动。
 */
int
main(int argc, char **argv)
{
  uint requests[DISKSCHED_MAX_REQUESTS];
  struct disksched_result result;
  uint start;
  uint max_block;
  int policy;
  int count;

  if(argc < 5){
    usage();
    exit(2);
  }
  count = argc - 4;
  if(count > DISKSCHED_MAX_REQUESTS){
    usage();
    exit(2);
  }

  policy = parse_policy(argv[1]);
  if(policy < 0 || parse_uint(argv[2], &start) < 0 ||
     parse_uint(argv[3], &max_block) < 0){
    usage();
    exit(2);
  }
  for(int i = 0; i < count; i++){
    if(parse_uint(argv[i + 4], &requests[i]) < 0){
      usage();
      exit(2);
    }
  }

  if(disksched_model(policy, start, max_block, requests, count, &result) < 0){
    fprintf(2, "disksched: request or boundary is outside the model range\n");
    exit(1);
  }

  printf("DISKMODEL policy=%s start=%d max=%d order=",
         disksched_policy_name(policy), start, max_block);
  print_order(&result);
  printf(" cost=%l unit=logical-block-distance\n", result.cost);
  printf("DISKMODEL boundary=teaching-only actual-device-order=opaque\n");
  exit(0);
}
