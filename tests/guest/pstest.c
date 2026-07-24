#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/riscv.h"
#include "kernel/memlayout.h"
#include "kernel/procinfo.h"
#include "user/user.h"
#include "user/paths.h"

#define OUTPUT_SIZE 8192
#define STATE_RETRY_TICKS 100

// 大型快照和捕获缓冲区放在 BSS，避免占用 xv6 单页用户栈。
static struct procinfo entries[NPROC];
static char output[OUTPUT_SIZE];

/**
 * 在断言失败时打印稳定诊断并终止测试。
 *
 * @param condition 非零表示断言成立。
 * @param message 失败时输出的场景说明。
 */
static void
check(int condition, char *message)
{
  if(condition)
    return;
  printf("pstest: %s\n", message);
  exit(1);
}

/**
 * 在当前快照中查找指定 PID。
 *
 * @param count entries 中的有效条目数量。
 * @param pid 目标进程 ID。
 * @return 找到时返回条目地址，否则返回 0。
 */
static struct procinfo *
find_process(int count, int pid)
{
  for(int i = 0; i < count; i++)
    if(entries[i].pid == pid)
      return &entries[i];
  return 0;
}

/**
 * 判断完整文本中是否包含指定连续子串。
 *
 * @param text 以 NUL 结尾的待搜索文本。
 * @param needle 以 NUL 结尾的非空目标文本。
 * @return 找到返回 1，否则返回 0。
 */
static int
contains(char *text, char *needle)
{
  for(int i = 0; text[i] != 0; i++){
    int j;
    for(j = 0; needle[j] != 0 && text[i + j] == needle[j]; j++)
      ;
    if(needle[j] == 0)
      return 1;
  }
  return 0;
}

/**
 * 在有限 tick 内等待目标进程进入指定公开状态。
 *
 * @param pid 目标进程 ID。
 * @param state 期望的 PROCINFO_STATE_*。
 * @return 观察到目标状态返回 1；超时或快照失败返回 0。
 */
static int
wait_for_state(int pid, int state)
{
  for(int attempt = 0; attempt < STATE_RETRY_TICKS; attempt++){
    int count = getprocs(entries, NPROC);
    if(count < 0)
      return 0;
    struct procinfo *process = find_process(count, pid);
    if(process != 0 && process->state == state)
      return 1;
    sleep(1);
  }
  return 0;
}

/** 验证调用进程自身以 RUNNING 状态出现在快照中。 */
static void
test_current_process(void)
{
  int count = getprocs(entries, NPROC);
  struct procinfo *current;

  check(count > 0, "getprocs returned no entries");
  current = find_process(count, getpid());
  check(current != 0, "current process missing");
  check(current->state == PROCINFO_STATE_RUNNING,
        "current process is not running");
  check(strcmp(current->name, "pstest") == 0,
        "current process name mismatch");
}

/** 验证无效容量和 supervisor-only 地址被系统调用拒绝。 */
static void
test_invalid_arguments(void)
{
  check(getprocs(entries, 0) < 0, "zero capacity accepted");
  check(getprocs((struct procinfo *)USERMAX, 1) < 0,
        "supervisor-only destination accepted");
}

/** 验证阻塞在 sleep() 中的子进程可被观察为 SLEEPING。 */
static void
test_sleeping_process(void)
{
  int ready[2];
  int pid;
  int status = 0;
  char marker;

  check(pipe(ready) == 0, "sleep pipe failed");
  pid = fork();
  check(pid >= 0, "sleep fork failed");
  if(pid == 0){
    close(ready[0]);
    check(write(ready[1], "r", 1) == 1, "sleep ready write failed");
    close(ready[1]);
    sleep(1000);
    exit(0);
  }

  close(ready[1]);
  check(read(ready[0], &marker, 1) == 1, "sleep ready read failed");
  close(ready[0]);
  check(wait_for_state(pid, PROCINFO_STATE_SLEEPING),
        "sleeping child not observed");
  check(kill(pid) == 0, "sleeping child kill failed");
  check(wait(&status) == pid, "sleeping child wait failed");
}

/** 验证父进程回收前的已退出子进程可被观察为 ZOMBIE。 */
static void
test_zombie_process(void)
{
  int pid;
  int status = 0;

  pid = fork();
  check(pid >= 0, "zombie fork failed");
  if(pid == 0)
    exit(0);

  check(wait_for_state(pid, PROCINFO_STATE_ZOMBIE),
        "zombie child not observed");
  check(wait(&status) == pid, "zombie child wait failed");
  check(status == 0, "zombie child status mismatch");
}

/**
 * 执行真实 /bin/ps，并捕获标准输出和错误输出。
 *
 * @return ps 的退出状态；基础设施失败时直接终止测试。
 */
static int
capture_ps(void)
{
  int pipefd[2];
  int pid;
  int status = 0;
  int total = 0;
  int count;
  char *argv[] = {XV6_BIN_PATH("ps"), 0};

  check(pipe(pipefd) == 0, "ps pipe failed");
  pid = fork();
  check(pid >= 0, "ps fork failed");
  if(pid == 0){
    close(pipefd[0]);
    close(1);
    check(dup(pipefd[1]) == 1, "redirect stdout failed");
    close(2);
    check(dup(pipefd[1]) == 2, "redirect stderr failed");
    close(pipefd[1]);
    exec(XV6_BIN_PATH("ps"), argv);
    exit(127);
  }

  close(pipefd[1]);
  while(total < sizeof(output) - 1){
    count = read(pipefd[0], output + total, sizeof(output) - total - 1);
    if(count <= 0)
      break;
    total += count;
  }
  output[total] = 0;
  close(pipefd[0]);
  check(wait(&status) == pid, "ps wait failed");
  return status;
}

/** 验证真实命令输出标题，并包含正在执行快照的 ps 自身。 */
static void
test_ps_command(void)
{
  check(capture_ps() == 0, "ps command failed");
  check(contains(output, "PID\tPPID\tSTATE\tNAME\n"),
        "ps header missing");
  check(contains(output, "\trunning\tps\n"),
        "ps process row missing");
}

int
main(void)
{
  test_current_process();
  test_invalid_arguments();
  test_sleeping_process();
  test_zombie_process();
  test_ps_command();
  printf("pstest: OK\n");
  exit(0);
}
