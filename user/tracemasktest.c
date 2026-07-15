#include "kernel/types.h"
#include "kernel/syscall.h"
#include "kernel/syscall_names.h"
#include "user/user.h"
#include "user/tracemask.h"

// 描述一条表驱动的 trace 参数解析测试用例。
struct trace_mask_case {
  char *spec;          // 传给 trace_parse_mask() 的输入字符串。
  int expected_status; // 预期返回的 enum trace_mask_status 状态。
  int expected_mask;   // 解析成功时预期得到的整数掩码。
};

/*
 * 执行具有代表性的成功和失败用例，并验证每个已注册系统调用名称都能
 * 正确映射回对应的 SYS_* 位。
 *
 * 参数：
 *   无。
 *
 * 返回值：
 *   所有断言通过时以状态码 0 退出；存在失败用例时以状态码 1 退出。
 */
int
main(void)
{
  // 固定用例覆盖名称组合、整数兼容、非法分隔符、未知名称和整数溢出。
  struct trace_mask_case cases[] = {
    {"read", TRACE_MASK_OK, 1U << SYS_read},
    {"read,write", TRACE_MASK_OK, (1U << SYS_read) | (1U << SYS_write)},
    {"read,read", TRACE_MASK_OK, 1U << SYS_read},
    {"32", TRACE_MASK_OK, 32},
    {"0", TRACE_MASK_OK, 0},
    {"", TRACE_MASK_EMPTY, 0},
    {"unknown", TRACE_MASK_UNKNOWN, 0},
    {"read,,write", TRACE_MASK_FORMAT, 0},
    {",read", TRACE_MASK_FORMAT, 0},
    {"read,", TRACE_MASK_FORMAT, 0},
    {"2147483648", TRACE_MASK_RANGE, 0},
    {"read|write", TRACE_MASK_UNKNOWN, 0},
  };
  int failed = 0; // 实际解析结果不符合预期的用例数量。
  int case_index; // 当前正在执行的固定测试用例下标。
  int syscall_number; // 当前根据共享名称表检查的 SYS_* 编号。

  for(case_index = 0;
      case_index < (int)(sizeof(cases) / sizeof(cases[0]));
      case_index++){
    int mask = -1; // 失败路径哨兵值，用于检查解析器是否错误写入输出参数。
    int status;    // 当前固定用例实际得到的解析状态。

    status = trace_parse_mask(cases[case_index].spec, &mask);
    if(status != cases[case_index].expected_status ||
       (status == TRACE_MASK_OK && mask != cases[case_index].expected_mask)){
      printf("tracemasktest: %s: status %d mask %d\n",
             cases[case_index].spec, status, mask);
      failed++;
    }
  }

  // 该循环防止新增系统调用只在内核中可打印，却遗漏用户态名称解析支持。
  for(syscall_number = 1;
      syscall_number < (int)SYSCALL_NAME_COUNT;
      syscall_number++){
    int mask = 0; // 根据当前已注册系统调用名称生成的实际掩码。
    int status;   // 当前已注册名称的实际解析状态。

    status = trace_parse_mask(syscall_names[syscall_number], &mask);
    if(status != TRACE_MASK_OK || mask != (int)(1U << syscall_number)){
      printf("tracemasktest: syscall %s: status %d mask %d\n",
             syscall_names[syscall_number], status, mask);
      failed++;
    }
  }

  if(failed){
    printf("tracemasktest: %d tests failed\n", failed);
    exit(1);
  }

  printf("tracemasktest: all tests passed\n");
  exit(0);
}