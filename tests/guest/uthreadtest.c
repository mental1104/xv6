#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "tests/guest/testlib.h"

#define OUTPUT_SIZE 8192

/**
 * 执行 Lab7 uthread 用户程序并验证三个线程都完成 100 次协作式调度。
 *
 * uthread 当前在最后一个线程退出后以 exit(-1) 结束，这是原实验程序用来
 * 表示“没有可运行线程”的既有行为。本测试同时检查该状态和完整输出，避免
 * 仅凭一行诊断将提前退出误判为通过。
 *
 * @return 本函数通过 exit() 返回：行为符合预期为 0，否则为 1。
 */
int
main(void)
{
  char *argv[] = {"uthread", 0};
  char *output = malloc(OUTPUT_SIZE);
  int status;

  if(output == 0){
    printf("uthreadtest: malloc failed\n");
    exit(1);
  }
  if(xv6_test_run_capture(argv, 0, output, OUTPUT_SIZE, &status) < 0){
    printf("uthreadtest: capture infrastructure failed\n");
    free(output);
    exit(1);
  }

  if(status != -1 ||
     !xv6_test_contains(output, "thread_a started\n") ||
     !xv6_test_contains(output, "thread_b started\n") ||
     !xv6_test_contains(output, "thread_c started\n") ||
     !xv6_test_contains(output, "thread_a: exit after 100\n") ||
     !xv6_test_contains(output, "thread_b: exit after 100\n") ||
     !xv6_test_contains(output, "thread_c: exit after 100\n") ||
     !xv6_test_contains(output, "thread_schedule: no runnable threads\n")){
    printf("uthreadtest: unexpected scheduling result status=%d\n%s", status, output);
    free(output);
    exit(1);
  }

  free(output);
  printf("uthreadtest: OK\n");
  exit(0);
}
