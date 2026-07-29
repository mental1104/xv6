#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/** 主线 xv6test 入口；Makefile 仅在该翻译单元内重命名 main。 */
int xv6test_original_main(int argc, char *argv[]);

/** 等待一个子进程，并把其退出状态返回给调用者。 */
static int
wait_child(int pid)
{
  int status = 1;

  if(pid < 0 || waitpid(pid, &status, 0) != pid)
    return 1;
  return status;
}

/** 在子进程中运行主线 xv6test，避免它的 exit() 终止适配入口。 */
static int
run_original(int argc, char *argv[])
{
  int pid = fork();

  if(pid < 0)
    return 1;
  if(pid == 0)
    xv6test_original_main(argc, argv);
  return wait_child(pid);
}

/** 通过稳定的 XV6TEST 协议运行事件循环回归。 */
static int
run_eventloop_test(void)
{
  char *argv[] = {"/eventlooptest", 0};
  int pid;
  int status;

  printf("XV6TEST begin group=core test=core-eventloop\n");
  printf("XV6TEST run 1 - core-eventloop group=core\n");
  pid = fork();
  if(pid < 0){
    printf("xv6test-eventloop: FAIL: fork\n");
    return 1;
  }
  if(pid == 0){
    exec(argv[0], argv);
    printf("xv6test-eventloop: FAIL: exec %s\n", argv[0]);
    exit(1);
  }

  status = wait_child(pid);
  if(status != 0){
    printf("xv6test-eventloop: FAIL: status=%d\n", status);
    printf("XV6TEST not ok 1 - core-eventloop status=%d\n", status);
    printf("XV6TEST summary selected=1 passed=0 failed=1\n");
    printf("XV6TEST done status=1\n");
    return 1;
  }

  printf("XV6TEST ok 1 - core-eventloop\n");
  printf("XV6TEST summary selected=1 passed=1 failed=0\n");
  printf("XV6TEST done status=0\n");
  return 0;
}

/** 判断当前选择是否必须追加事件循环回归。 */
static int
should_append_eventloop(int argc, char *argv[])
{
  if(argc == 1)
    return 1;
  if(argc == 2 && strcmp(argv[1], "--all") == 0)
    return 1;
  return argc == 3 && strcmp(argv[1], "--group") == 0 &&
         strcmp(argv[2], "core") == 0;
}

/** 保留主线注册表，只在 core 入口和精确选择时追加 eventlooptest。 */
int
main(int argc, char *argv[])
{
  if(argc == 3 && strcmp(argv[1], "--run") == 0 &&
     strcmp(argv[2], "core-eventloop") == 0)
    exit(run_eventloop_test());

  if(should_append_eventloop(argc, argv)){
    int status = run_original(argc, argv);

    if(status != 0)
      exit(status);
    exit(run_eventloop_test());
  }

  xv6test_original_main(argc, argv);
  exit(1);
}
