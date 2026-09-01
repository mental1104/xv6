#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/riscv.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "tests/guest/testlib.h"

// 捕获事件循环输出的缓冲区放在 BSS，避免占用 xv6 单页用户栈。
static char captured_output[4096];

/** 输出稳定失败原因并以非零状态终止测试。 */
static void
fail(char *message)
{
  printf("eventlooptest: FAIL: %s\n", message);
  exit(1);
}

/** 断言一个测试条件。 */
static void
check(int condition, char *message)
{
  if(!condition)
    fail(message);
}

/** 等待指定子进程成功退出。 */
static void
wait_successfully(int pid)
{
  int status = 0;

  check(waitpid(pid, &status, 0) == pid, "waitpid returned unexpected child");
  check(status == 0, "child process exited with failure");
}

/** 验证非阻塞快照、多源位图、重复等待与 pipe EOF 就绪语义。 */
static void
test_readiness_snapshot(void)
{
  int first[2];
  int second[2];
  int fds[2];
  char value;

  check(pipe(first) == 0, "cannot create first readiness pipe");
  check(pipe(second) == 0, "cannot create second readiness pipe");
  fds[0] = first[0];
  fds[1] = second[0];

  check(pollread(fds, 2, 0) == 0, "empty pipes were reported ready");
  check(write(first[1], "a", 1) == 1, "cannot write first event");
  check(write(second[1], "b", 1) == 1, "cannot write second event");
  check(pollread(fds, 2, 0) == 3, "multi-source readiness bitmap is wrong");

  check(read(first[0], &value, 1) == 1 && value == 'a',
        "cannot consume first event");
  check(read(second[0], &value, 1) == 1 && value == 'b',
        "cannot consume second event");
  check(pollread(fds, 2, 0) == 0, "consumed pipes remained data-ready");

  close(first[1]);
  check(pollread(fds, 2, 0) == 1, "writer close did not expose EOF readiness");
  check(read(first[0], &value, 1) == 0, "EOF-ready pipe did not return zero");
  close(first[0]);

  fds[0] = second[0];
  check(pollread(fds, 1, 0) == 0, "remaining empty pipe was reported ready");
  check(write(second[1], "c", 1) == 1, "cannot write repeated event");
  check(pollread(fds, 1, 0) == 1, "repeated event was omitted");
  check(read(second[0], &value, 1) == 1 && value == 'c',
        "cannot consume repeated event");

  close(second[1]);
  check(pollread(fds, 1, 0) == 1, "second EOF was not reported ready");
  check(read(second[0], &value, 1) == 0, "second EOF did not return zero");
  close(second[0]);
}

/** 验证扫描与 sleep 边界附近的写入唤醒不会丢失。 */
static void
test_blocking_wait(void)
{
  int event_pipe[2];
  int fds[1];
  int pid;
  char value;

  check(pipe(event_pipe) == 0, "cannot create blocking wait pipe");
  pid = fork();
  check(pid >= 0, "cannot fork blocking wait producer");
  if(pid == 0){
    close(event_pipe[0]);
    sleep(3);
    if(write(event_pipe[1], "w", 1) != 1)
      exit(1);
    close(event_pipe[1]);
    exit(0);
  }

  close(event_pipe[1]);
  fds[0] = event_pipe[0];
  check(pollread(fds, 1, 1) == 1, "blocking wait missed producer event");
  check(read(event_pipe[0], &value, 1) == 1 && value == 'w',
        "blocking wait event cannot be consumed");
  check(pollread(fds, 1, 1) == 1, "blocking wait missed writer EOF");
  check(read(event_pipe[0], &value, 1) == 0, "writer EOF did not return zero");
  close(event_pipe[0]);
  wait_successfully(pid);
}

/** 验证教学接口拒绝无法诚实表达的对象和参数。 */
static void
test_invalid_inputs(void)
{
  int endpoints[2];
  int fds[1];

  check(pipe(endpoints) == 0, "cannot create invalid-input pipe");

  fds[0] = endpoints[1];
  check(pollread(fds, 1, 0) == -1, "pipe write end was accepted as readable");
  fds[0] = NOFILE;
  check(pollread(fds, 1, 0) == -1, "out-of-range fd was accepted");
  fds[0] = endpoints[0];
  check(pollread(fds, 0, 0) == -1, "zero descriptor count was accepted");
  check(pollread(fds, NOFILE + 1, 0) == -1,
        "descriptor count beyond NOFILE was accepted");
  check(pollread(fds, 1, 2) == -1, "invalid wait mode was accepted");
  check(pollread((int *)MAXVA, 1, 0) == -1,
        "unmapped user array was accepted");

  close(endpoints[0]);
  close(endpoints[1]);
}

/** 执行事件循环用户程序并验证退出状态与关键公开 oracle。 */
static void
run_eventloop_and_check(char **argv, char *required_first, char *required_second)
{
  int status = 0;

  check(xv6_test_run_capture(argv, 0, captured_output,
                             sizeof(captured_output), &status) == 0,
        "cannot execute eventloop program");
  check(status == 0, "eventloop program exited with failure");
  check(xv6_test_contains(captured_output, "EVENTLOOP snapshot ready=0"),
        "eventloop omitted nonblocking snapshot");
  check(xv6_test_contains(captured_output,
                          "ORACLE state-complete PASS a=2 b=1 total=3"),
        "eventloop state machine oracle failed");
  check(xv6_test_contains(captured_output, required_first),
        "eventloop omitted mode-specific oracle");
  check(xv6_test_contains(captured_output, required_second),
        "eventloop omitted completion marker");
}

/** 黑盒验证普通模式、阻塞 handler 反例和参数错误路径。 */
static void
test_eventloop_program(void)
{
  char *fast_argv[] = {"eventloop", 0};
  char *slow_argv[] = {"eventloop", "--slow", 0};
  char *bad_argv[] = {"eventloop", "--unknown", 0};
  int status = 0;

  run_eventloop_and_check(fast_argv,
                          "ORACLE slow-handler SKIP",
                          "EVENTLOOP done mode=fast");
  run_eventloop_and_check(slow_argv,
                          "ORACLE slow-handler PASS source=B",
                          "EVENTLOOP done mode=slow");

  check(xv6_test_run_capture(bad_argv, 0, captured_output,
                             sizeof(captured_output), &status) == 0,
        "cannot execute eventloop argument error path");
  check(status == 2, "eventloop argument error returned wrong status");
  check(xv6_test_contains(captured_output, "Usage: eventloop [--slow]"),
        "eventloop argument error omitted usage");
}

/** 运行 pipe readiness 与事件循环的完整 guest 回归。 */
int
main(void)
{
  test_readiness_snapshot();
  test_blocking_wait();
  test_invalid_inputs();
  test_eventloop_program();
  printf("eventlooptest: OK\n");
  exit(0);
}
