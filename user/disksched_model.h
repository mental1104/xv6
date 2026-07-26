#ifndef XV6_USER_DISKSCHED_MODEL_H
#define XV6_USER_DISKSCHED_MODEL_H

#define DISKSCHED_MAX_REQUESTS 32

enum disksched_policy {
  DISKSCHED_POLICY_FCFS = 1,
  DISKSCHED_POLICY_SSTF = 2,
  DISKSCHED_POLICY_SCAN = 3,
};

/** 保存教学调度模型选择出的请求顺序和抽象移动成本。 */
struct disksched_result {
  int count;
  uint order[DISKSCHED_MAX_REQUESTS];
  uint64 cost;
};

int disksched_model(int policy, uint start, uint max_block,
                    const uint *requests, int count,
                    struct disksched_result *result);
char *disksched_policy_name(int policy);

#endif
