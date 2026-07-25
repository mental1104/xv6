#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/paths.h"
#include "tests/guest/testlib.h"

#define OUTPUT_SIZE 4096

/**
 * 执行可抢占 uthread 用户程序，并验证线程 API 与调度生命周期闭环。
 *
 * 子程序必须正常退出，同时输出八条稳定结果：工作线程与主线程共享地址空间和
 * PID；入口参数按借用指针传递；self/duplicate/invalid join 明确失败；两个线程
 * 使用不同栈且局部状态跨切换保持；不主动 yield 的线程仍可抢占；16 个工作槽位
 * 可用且第 17 次创建失败；退出槽位可连续三轮复用；最终主线程能够正常收尾。
 * 完整输出检查可避免只完成部分阶段的假阳性。
 *
 * @return 本函数通过 exit() 返回：全部结果符合预期为 0，否则为 1。
 */
int
main(void)
{
  char *argv[] = {XV6_USR_BIN_PATH("uthread"), 0};
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

  if(status != 0 ||
     !xv6_test_contains(output, "uthread: shared-address OK same-pid=1 shared=43\n") ||
     !xv6_test_contains(output, "uthread: argument-lifetime OK observed=42 ownership=borrowed\n") ||
     !xv6_test_contains(output, "uthread: join-failures OK self=-1 duplicate=-1 invalid=-1\n") ||
     !xv6_test_contains(output, "uthread: stack-context OK distinct=1 preserved=2\n") ||
     !xv6_test_contains(output, "uthread: preempt OK\n") ||
     !xv6_test_contains(output, "uthread: capacity OK created=16 overflow=-1\n") ||
     !xv6_test_contains(output, "uthread: lifecycle OK rounds=3 completed=48\n") ||
     !xv6_test_contains(output, "uthread: all tests OK\n")){
    printf("uthreadtest: unexpected scheduling result status=%d\n%s", status, output);
    free(output);
    exit(1);
  }

  free(output);
  printf("uthreadtest: OK\n");
  exit(0);
}
