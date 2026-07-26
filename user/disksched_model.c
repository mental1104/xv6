#include "kernel/types.h"
#include "user/disksched_model.h"

/** 返回两个模型块号之间的绝对距离。 */
static uint64
block_distance(uint left, uint right)
{
  return left >= right ? (uint64)left - right : (uint64)right - left;
}

/** 使用插入排序生成升序请求副本，避免修改调用者输入。 */
static void
sort_requests(const uint *requests, int count, uint *sorted)
{
  for(int i = 0; i < count; i++){
    uint value = requests[i];
    int position = i;
    while(position > 0 && sorted[position - 1] > value){
      sorted[position] = sorted[position - 1];
      position--;
    }
    sorted[position] = value;
  }
}

/** 将一个请求追加到结果，并累计从当前位置到该请求的模型距离。 */
static void
append_request(struct disksched_result *result, uint *current, uint request)
{
  result->cost += block_distance(*current, request);
  *current = request;
  result->order[result->count++] = request;
}

/** 按输入顺序执行请求，建立不重排的 FCFS 基线。 */
static void
schedule_fcfs(uint start, const uint *requests, int count,
              struct disksched_result *result)
{
  uint current = start;

  for(int i = 0; i < count; i++)
    append_request(result, &current, requests[i]);
}

/** 每轮选择离当前位置最近的未完成请求；相同距离保持原输入顺序。 */
static void
schedule_sstf(uint start, const uint *requests, int count,
              struct disksched_result *result)
{
  int used[DISKSCHED_MAX_REQUESTS] = {0};
  uint current = start;

  for(int selected = 0; selected < count; selected++){
    int best = -1;
    uint64 best_distance = 0;

    for(int i = 0; i < count; i++){
      if(used[i])
        continue;
      uint64 distance = block_distance(current, requests[i]);
      if(best < 0 || distance < best_distance){
        best = i;
        best_distance = distance;
      }
    }

    used[best] = 1;
    append_request(result, &current, requests[best]);
  }
}

/**
 * 先向大块号方向扫描，再到达模型边界后反向处理较小块号请求。
 *
 * max_block 是教学模型的逻辑边界，不代表 virtio 或宿主磁盘真实几何。只有存在
 * 反向请求时才把到达边界的移动计入成本，结果顺序中不插入伪请求。
 */
static void
schedule_scan(uint start, uint max_block, const uint *requests, int count,
              struct disksched_result *result)
{
  uint sorted[DISKSCHED_MAX_REQUESTS];
  uint current = start;
  int split = 0;

  sort_requests(requests, count, sorted);
  while(split < count && sorted[split] < start)
    split++;

  for(int i = split; i < count; i++)
    append_request(result, &current, sorted[i]);

  if(split > 0){
    result->cost += block_distance(current, max_block);
    current = max_block;
    for(int i = split - 1; i >= 0; i--)
      append_request(result, &current, sorted[i]);
  }
}

/** 返回稳定策略名称，未知编号返回 unknown。 */
char *
disksched_policy_name(int policy)
{
  switch(policy){
  case DISKSCHED_POLICY_FCFS:
    return "fcfs";
  case DISKSCHED_POLICY_SSTF:
    return "sstf";
  case DISKSCHED_POLICY_SCAN:
    return "scan";
  default:
    return "unknown";
  }
}

/**
 * disksched_model 在纯用户态比较三种机械磁盘教学策略。
 *
 * @param policy DISKSCHED_POLICY_*。
 * @param start 模型磁头起始块号。
 * @param max_block SCAN 的模型右边界。
 * @param requests 待处理逻辑块号数组。
 * @param count 请求数量，范围为 1..DISKSCHED_MAX_REQUESTS。
 * @param result 成功时接收顺序和抽象移动成本；失败时保持原值。
 * @return 参数合法时返回 0，否则返回 -1。
 *
 * 该函数不参与 xv6 的真实 virtio I/O 路径，cost 仅为逻辑块号距离之和，不能解释
 * QEMU 延迟、宿主设备寻道或控制器内部完成顺序。
 */
int
disksched_model(int policy, uint start, uint max_block,
                const uint *requests, int count,
                struct disksched_result *result)
{
  struct disksched_result next = {0};

  if(result == 0 || requests == 0 || count <= 0 ||
     count > DISKSCHED_MAX_REQUESTS || start > max_block)
    return -1;
  if(policy != DISKSCHED_POLICY_FCFS &&
     policy != DISKSCHED_POLICY_SSTF &&
     policy != DISKSCHED_POLICY_SCAN)
    return -1;
  for(int i = 0; i < count; i++)
    if(requests[i] > max_block)
      return -1;

  switch(policy){
  case DISKSCHED_POLICY_FCFS:
    schedule_fcfs(start, requests, count, &next);
    break;
  case DISKSCHED_POLICY_SSTF:
    schedule_sstf(start, requests, count, &next);
    break;
  case DISKSCHED_POLICY_SCAN:
    schedule_scan(start, max_block, requests, count, &next);
    break;
  }

  *result = next;
  return 0;
}
