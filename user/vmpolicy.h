#ifndef XV6_USER_VMPOLICY_H
#define XV6_USER_VMPOLICY_H

#include "kernel/types.h"

#define VM_POLICY_MAX_FRAMES 4
#define VM_POLICY_MAX_PAGES 8
#define VM_POLICY_MAX_ACCESSES 24

/** 教学实验支持的受害页选择策略。 */
enum vm_policy_kind {
  VM_POLICY_FIFO = 0,
  VM_POLICY_CLOCK = 1,
};

/** 描述一次逻辑页面访问及其是否修改页面内容。 */
struct vm_policy_access {
  int page;
  int write;
};

/** 描述一组由实验自动执行的确定性页面访问。 */
struct vm_policy_workload {
  char *label;
  int page_count;
  int frame_count;
  int access_count;
  struct vm_policy_access accesses[VM_POLICY_MAX_ACCESSES];
};

/** 记录一次策略决策前后的可观察状态。 */
struct vm_policy_trace {
  int page;
  int write;
  int hit;
  int victim;
  int scanned;
  int hand_before;
  int hand_after;
  uint resident_mask;
  uint referenced_mask;
};

/** 汇总一轮策略实验的决策与真实 swap 机制计数。 */
struct vm_policy_result {
  int accesses;
  int hits;
  int misses;
  int evictions;
  int page_outs;
  int page_ins;
  int error;
  int error_step;
  uint final_resident_mask;
  struct vm_policy_trace trace[VM_POLICY_MAX_ACCESSES];
};

enum vm_policy_error {
  VM_POLICY_ERROR_NONE = 0,
  VM_POLICY_ERROR_ARGUMENT = 1,
  VM_POLICY_ERROR_SBRK = 2,
  VM_POLICY_ERROR_SWAPINFO = 3,
  VM_POLICY_ERROR_STATE = 4,
  VM_POLICY_ERROR_SWAPOUT = 5,
  VM_POLICY_ERROR_DATA = 6,
  VM_POLICY_ERROR_COUNTER = 7,
  VM_POLICY_ERROR_CLEANUP = 8,
};

/**
 * 执行一组确定性访问，并让用户态策略通过 swapout() 驱动真实换出机制。
 *
 * @param kind FIFO 或 CLOCK。
 * @param workload 访问序列、逻辑页数与可用页框数。
 * @param result 接收逐步 trace、命中/缺页和真实 page-in/page-out 计数。
 * @return 全部状态与资源 oracle 成立时返回 0，否则返回 -1 并设置 result->error。
 */
int vm_policy_run(enum vm_policy_kind kind,
                  struct vm_policy_workload *workload,
                  struct vm_policy_result *result);

/** 返回稳定的策略名称，用于实验输出和测试诊断。 */
char *vm_policy_name(enum vm_policy_kind kind);

/** 返回稳定的错误名称，用于定位失败责任阶段。 */
char *vm_policy_error_name(int error);

/** 打印策略输入、逐步选择和最终机制计数。 */
void vm_policy_print(struct vm_policy_workload *workload,
                     enum vm_policy_kind kind,
                     struct vm_policy_result *result);

#endif
