#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "kernel/memviz.h"
#include "user/user.h"
#include "user/paths.h"

static char output[32768];
static struct memviz_snapshot baseline;
static struct memviz_snapshot observed;

/** 输出失败原因并以非零状态终止测试。 */
static void
fail(char *message)
{
  fprintf(2, "memtargettest: FAIL: %s\n", message);
  exit(1);
}

/** 判断完整输出中是否包含指定稳定片段。 */
static int
text_contains(char *text, char *pattern)
{
  for(int i = 0; text[i] != 0; i++){
    int j = 0;
    while(pattern[j] != 0 && text[i + j] == pattern[j])
      j++;
    if(pattern[j] == 0)
      return 1;
  }
  return 0;
}

/** 将正整数 PID 写成十进制字符串。 */
static void
format_pid(int pid, char *buffer)
{
  char reversed[16];
  int count = 0;

  do {
    reversed[count++] = '0' + pid % 10;
    pid /= 10;
  } while(pid > 0);
  for(int i = 0; i < count; i++)
    buffer[i] = reversed[count - 1 - i];
  buffer[count] = 0;
}

/**
 * 执行用户程序并捕获标准输出。
 *
 * @param path 镜像内绝对程序路径。
 * @param argv 传给 exec() 的参数向量。
 * @param quiet_stderr 非零时关闭子进程标准错误。
 * @return 子进程退出状态。
 */
static int
run_and_capture(char *path, char **argv, int quiet_stderr)
{
  int fds[2];
  if(pipe(fds) < 0)
    fail("capture pipe");

  int pid = fork();
  if(pid < 0)
    fail("capture fork");
  if(pid == 0){
    close(fds[0]);
    close(1);
    if(dup(fds[1]) != 1)
      exit(1);
    close(fds[1]);
    if(quiet_stderr)
      close(2);
    exec(path, argv);
    exit(1);
  }

  close(fds[1]);
  int total = 0;
  while(total < (int)sizeof(output) - 1){
    int count = read(fds[0], output + total, sizeof(output) - 1 - total);
    if(count < 0)
      fail("capture read");
    if(count == 0)
      break;
    total += count;
  }
  close(fds[0]);
  output[total] = 0;

  int status = -1;
  if(wait(&status) != pid)
    fail("capture wait");
  return status;
}

/** 验证 memtarget 正常生命周期和参数错误都可靠传播退出状态。 */
static void
test_memtarget_cli(void)
{
  char *valid[] = {
    XV6_USR_BIN_PATH("memtarget"), "2", "1", "--exit", 0
  };
  if(run_and_capture(valid[0], valid, 0) != 0)
    fail("valid lifecycle exit status");
  if(!text_contains(output, "memtarget: pid=") ||
     !text_contains(output, "page=1/2 state=lazy") ||
     !text_contains(output, "page=1/2 state=resident") ||
     !text_contains(output, "page=2/2 state=lazy") ||
     !text_contains(output, "page=2/2 state=resident") ||
     !text_contains(output, "state=complete pages=2"))
    fail("lifecycle output");

  char *zero_pages[] = {
    XV6_USR_BIN_PATH("memtarget"), "0", "1", "--exit", 0
  };
  if(run_and_capture(zero_pages[0], zero_pages, 1) == 0)
    fail("zero pages accepted");

  char *zero_interval[] = {
    XV6_USR_BIN_PATH("memtarget"), "1", "0", "--exit", 0
  };
  if(run_and_capture(zero_interval[0], zero_interval, 1) == 0)
    fail("zero interval accepted");

  printf("memtargettest: helper lifecycle OK\n");
}

/**
 * 创建一个扩展两页后阻塞在 pipe read 的稳定目标。
 *
 * @param ready 子进程写入一个字节表示地址空间已经准备完成。
 * @param release_pipe 父进程写入一个字节后允许子进程退出。
 * @return 父进程中返回目标 PID；子进程不返回。
 */
static int
spawn_stable_target(int ready[2], int release_pipe[2])
{
  if(pipe(ready) < 0 || pipe(release_pipe) < 0)
    fail("target pipes");

  int pid = fork();
  if(pid < 0)
    fail("target fork");
  if(pid == 0){
    close(ready[0]);
    close(release_pipe[1]);

    char *base = sbrk(2 * PGSIZE);
    if(base == (char *)-1)
      exit(1);
    base[0] = 7;
    if(write(ready[1], "R", 1) != 1)
      exit(1);
    close(ready[1]);

    char token;
    if(read(release_pipe[0], &token, 1) != 1)
      exit(1);
    close(release_pipe[0]);
    exit(0);
  }

  close(ready[1]);
  close(release_pipe[0]);
  return pid;
}

/** 多核调度下短暂重试，直到目标进入允许稳定采样的非 RUNNING 状态。 */
static int
snapshot_pid_retry(int pid, int view, struct memviz_snapshot *snapshot)
{
  for(int attempt = 0; attempt < 100; attempt++){
    if(memsnapshot_pid(pid, view, snapshot) == 0)
      return 0;
    sleep(1);
  }
  return -1;
}

/** 验证系统调用拒绝非法 PID，并保留显式观察当前进程的兼容路径。 */
static void
test_pid_arguments(void)
{
  if(memsnapshot_pid(0, MEMVIZ_VIEW_USER, &observed) != -1 ||
     memsnapshot_pid(-1, MEMVIZ_VIEW_USER, &observed) != -1 ||
     memsnapshot_pid(0x7fffffff, MEMVIZ_VIEW_USER, &observed) != -1)
    fail("invalid target pid accepted");
  if(memsnapshot_pid(getpid(), MEMVIZ_VIEW_USER, &observed) < 0 ||
     observed.process_pid != getpid())
    fail("explicit self snapshot");

  printf("memtargettest: pid arguments OK\n");
}

/** 验证 sleeping 目标的 user/kernel/pagetable 快照、CLI 输出和回收边界。 */
static void
test_stable_target_observation(void)
{
  if(memsnapshot(MEMVIZ_VIEW_USER, &baseline) < 0)
    fail("parent baseline");

  int ready[2];
  int release_pipe[2];
  int pid = spawn_stable_target(ready, release_pipe);

  char token;
  if(read(ready[0], &token, 1) != 1)
    fail("target ready");
  close(ready[0]);

  if(snapshot_pid_retry(pid, MEMVIZ_VIEW_USER, &observed) < 0)
    fail("target user snapshot");
  if(observed.process_pid != pid)
    fail("target pid identity");
  if(observed.process_size != baseline.process_size + 2 * PGSIZE)
    fail("target process size");
  if(observed.dynamic_page_count != baseline.dynamic_page_count + 2)
    fail("target dynamic page count");
  if(observed.dynamic_resident_pages < baseline.dynamic_resident_pages + 1)
    fail("target resident page missing");
  if(observed.dynamic_lazy_pages < baseline.dynamic_lazy_pages + 1)
    fail("target lazy page missing");

  if(snapshot_pid_retry(pid, MEMVIZ_VIEW_KERNEL, &observed) < 0 ||
     observed.process_pid != pid || !observed.kernel_stack_valid)
    fail("target kernel snapshot");
  if(snapshot_pid_retry(pid, MEMVIZ_VIEW_PAGETABLE, &observed) < 0 ||
     observed.process_pid != pid || observed.user_pagetable == 0)
    fail("target pagetable snapshot");

  char pid_text[16];
  format_pid(pid, pid_text);
  char *memviz_argv[] = {
    XV6_USR_BIN_PATH("memviz"), "user", "--pid", pid_text, "--plain", 0
  };
  if(run_and_capture(memviz_argv[0], memviz_argv, 0) != 0)
    fail("memviz pid execution");
  if(!text_contains(output, "observed process pid=") ||
     !text_contains(output, pid_text) ||
     !text_contains(output, "DYNAMIC EXTENT / page states"))
    fail("memviz pid output");

  if(write(release_pipe[1], "X", 1) != 1)
    fail("target release");
  close(release_pipe[1]);
  int status = -1;
  if(wait(&status) != pid || status != 0)
    fail("target exit");
  if(memsnapshot_pid(pid, MEMVIZ_VIEW_USER, &observed) != -1)
    fail("reaped target remains observable");

  printf("memtargettest: stable target observation OK\n");
}

int
main(void)
{
  test_memtarget_cli();
  test_pid_arguments();
  test_stable_target_observation();
  printf("memtargettest: OK\n");
  exit(0);
}
