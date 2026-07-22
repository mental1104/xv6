#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "tests/guest/testlib.h"

#define OUTPUT_SIZE 2048

/**
 * 输出统一的 Lab1 测试失败诊断并结束当前测试进程。
 *
 * @param name 当前子测试名称。
 * @param reason 稳定、简短的失败原因。
 * @param output 被测程序输出；为空时不追加输出内容。
 * @return 不返回，始终通过 exit(1) 结束。
 */
static void
fail(char *name, char *reason, char *output)
{
  printf("lab1test: %s: %s\n", name, reason);
  if(output != 0 && output[0] != 0)
    printf("lab1test output:\n%s", output);
  exit(1);
}

/**
 * 执行 sleep 并验证至少经过请求的 tick 数。
 *
 * @return 通过时正常返回，失败时由 fail() 结束进程。
 */
static void
sleep_test(void)
{
  char *argv[] = {"sleep", "2", 0};
  char output[OUTPUT_SIZE];
  int status;
  int start = uptime();

  if(xv6_test_run_capture(argv, 0, output, sizeof(output), &status) < 0)
    fail("sleep", "capture infrastructure failed", output);
  if(status != 0)
    fail("sleep", "program returned non-zero", output);
  if(uptime() - start < 2)
    fail("sleep", "returned before requested ticks elapsed", output);
}

/**
 * 执行 pingpong 并验证父子两个方向都完成消息传递。
 *
 * @return 通过时正常返回，失败时由 fail() 结束进程。
 */
static void
pingpong_test(void)
{
  char *argv[] = {"pingpong", 0};
  char output[OUTPUT_SIZE];
  int status;

  if(xv6_test_run_capture(argv, 0, output, sizeof(output), &status) < 0)
    fail("pingpong", "capture infrastructure failed", output);
  if(status != 0)
    fail("pingpong", "program returned non-zero", output);
  if(!xv6_test_contains(output, "received ping") ||
     !xv6_test_contains(output, "received pong"))
    fail("pingpong", "missing ping or pong observation", output);
}

/**
 * 执行 primes 并检查筛选链的首尾代表性结果。
 *
 * @return 通过时正常返回，失败时由 fail() 结束进程。
 */
static void
primes_test(void)
{
  char *argv[] = {"primes", 0};
  char output[OUTPUT_SIZE];
  int status;

  if(xv6_test_run_capture(argv, 0, output, sizeof(output), &status) < 0)
    fail("primes", "capture infrastructure failed", output);
  if(status != 0)
    fail("primes", "program returned non-zero", output);
  if(!xv6_test_contains(output, "prime 2\n") ||
     !xv6_test_contains(output, "prime 31\n"))
    fail("primes", "missing expected boundary primes", output);
}

/**
 * 创建最小目录树，执行 find，并验证目标路径被输出。
 *
 * @return 通过时正常返回，失败时由 fail() 结束进程；测试结束会清理文件。
 */
static void
find_test(void)
{
  char *argv[] = {"find", ".", "needle", 0};
  char output[OUTPUT_SIZE];
  int fd;
  int status;

  // 上一轮异常退出可能留下同名资源；先做幂等清理再构造本轮场景。
  unlink("xv6test-find/needle");
  unlink("xv6test-find");
  if(mkdir("xv6test-find") < 0)
    fail("find", "mkdir failed", 0);
  fd = open("xv6test-find/needle", O_CREATE | O_WRONLY);
  if(fd < 0){
    unlink("xv6test-find");
    fail("find", "create target failed", 0);
  }
  if(write(fd, "x", 1) != 1){
    close(fd);
    unlink("xv6test-find/needle");
    unlink("xv6test-find");
    fail("find", "write target failed", 0);
  }
  close(fd);

  if(xv6_test_run_capture(argv, 0, output, sizeof(output), &status) < 0){
    unlink("xv6test-find/needle");
    unlink("xv6test-find");
    fail("find", "capture infrastructure failed", output);
  }

  unlink("xv6test-find/needle");
  unlink("xv6test-find");
  if(status != 0)
    fail("find", "program returned non-zero", output);
  if(!xv6_test_contains(output, "./xv6test-find/needle\n"))
    fail("find", "target path not found", output);
}

/**
 * 向 xargs 标准输入写入一行参数，并验证它拼接后执行目标命令。
 *
 * @return 通过时正常返回，失败时由 fail() 结束进程。
 */
static void
xargs_test(void)
{
  char *argv[] = {"xargs", "echo", "bye", 0};
  char output[OUTPUT_SIZE];
  int status;

  if(xv6_test_run_capture(argv, "hello too\n", output, sizeof(output), &status) < 0)
    fail("xargs", "capture infrastructure failed", output);
  if(status != 0)
    fail("xargs", "program returned non-zero", output);
  if(!xv6_test_contains(output, "bye hello too\n"))
    fail("xargs", "unexpected command expansion", output);
}

/**
 * 根据命令行参数执行一个 Lab1 用户程序黑盒测试。
 *
 * @param argc 必须为 2。
 * @param argv argv[1] 必须是 sleep、pingpong、primes、find 或 xargs。
 * @return 本函数通过 exit() 返回：测试通过为 0，测试失败为 1，参数错误为 2。
 */
int
main(int argc, char *argv[])
{
  if(argc != 2){
    fprintf(2, "Usage: lab1test sleep|pingpong|primes|find|xargs\n");
    exit(2);
  }

  if(strcmp(argv[1], "sleep") == 0)
    sleep_test();
  else if(strcmp(argv[1], "pingpong") == 0)
    pingpong_test();
  else if(strcmp(argv[1], "primes") == 0)
    primes_test();
  else if(strcmp(argv[1], "find") == 0)
    find_test();
  else if(strcmp(argv[1], "xargs") == 0)
    xargs_test();
  else {
    fprintf(2, "lab1test: unknown test %s\n", argv[1]);
    exit(2);
  }

  printf("lab1test: %s: OK\n", argv[1]);
  exit(0);
}
