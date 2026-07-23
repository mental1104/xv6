#include "kernel/types.h"
#include "kernel/wait.h"
#include "kernel/jobctl.h"
#include "user/user.h"
#include "tests/guest/test_assert.h"

/** 子进程组向测试父进程报告的 PGID 继承快照。 */
struct group_report {
  int leader_pid;
  int leader_pgid;
  int worker_pid;
  int worker_pgid;
};

/**
 * 作为进程组 leader 创建一个 worker，并保持两者可被 STOP/CONT/TERM 控制。
 *
 * @param report_fd 写端，用于向测试父进程交付固定快照。
 */
static void
group_leader(int report_fd)
{
  struct group_report report;
  int worker;

  if(setpgid(0, 0) < 0)
    exit(1);
  worker = fork();
  if(worker < 0)
    exit(1);
  if(worker == 0){
    for(;;)
      sleep(1000);
  }

  report.leader_pid = getpid();
  report.leader_pgid = getpgid(0);
  report.worker_pid = worker;
  report.worker_pgid = getpgid(worker);
  if(write(report_fd, &report, sizeof(report)) != sizeof(report))
    exit(1);
  close(report_fd);

  for(;;)
    sleep(1000);
}

/**
 * 验证 PGID、停止/继续事件、停止态终止回收和后台控制台读取隔离。
 */
static void
run_jobctl_test(void)
{
  int report_pipe[2];
  int result_pipe[2];
  int leader;
  int status = 0;
  struct group_report report;

  ASSERT_EQ(0, pipe(report_pipe));
  leader = fork();
  ASSERT_TRUE(leader > 0);
  if(leader == 0){
    close(report_pipe[0]);
    group_leader(report_pipe[1]);
  }

  close(report_pipe[1]);
  EXPECT_EQ(0, setpgid(leader, leader));
  ASSERT_EQ(sizeof(report), read(report_pipe[0], &report, sizeof(report)));
  close(report_pipe[0]);

  EXPECT_EQ(leader, report.leader_pid);
  EXPECT_EQ(leader, report.leader_pgid);
  EXPECT_TRUE(report.worker_pid > 0);
  EXPECT_EQ(leader, report.worker_pgid);
  EXPECT_EQ(leader, getpgid(leader));

  ASSERT_EQ(0, procctl(leader, JOBCTL_STOP));
  ASSERT_EQ(leader, waitpid(leader, &status, WUNTRACED));
  EXPECT_TRUE(WIFSTOPPED(status));
  EXPECT_EQ(0, waitpid(leader, &status, WNOHANG | WUNTRACED));

  ASSERT_EQ(0, procctl(leader, JOBCTL_CONT));
  ASSERT_EQ(leader, waitpid(leader, &status, WCONTINUED));
  EXPECT_TRUE(WIFCONTINUED(status));
  EXPECT_EQ(0, waitpid(leader, &status, WNOHANG | WCONTINUED));

  ASSERT_EQ(0, procctl(leader, JOBCTL_STOP));
  ASSERT_EQ(leader, waitpid(leader, &status, WUNTRACED));
  EXPECT_TRUE(WIFSTOPPED(status));
  ASSERT_EQ(0, procctl(leader, JOBCTL_TERM));
  ASSERT_EQ(leader, waitpid(leader, &status, 0));
  EXPECT_EQ(-1, getpgid(leader));

  // 当前测试程序属于前台组；新建子进程组读取 console 必须立即失败。
  ASSERT_EQ(0, pipe(result_pipe));
  int reader = fork();
  ASSERT_TRUE(reader > 0);
  if(reader == 0){
    char byte = 0;
    close(result_pipe[0]);
    if(setpgid(0, 0) < 0)
      exit(1);
    int count = read(0, &byte, 1);
    write(result_pipe[1], &count, sizeof(count));
    close(result_pipe[1]);
    exit(count == -1 ? 0 : 1);
  }
  close(result_pipe[1]);
  EXPECT_EQ(0, setpgid(reader, reader));
  int read_result = 0;
  ASSERT_EQ(sizeof(read_result),
            read(result_pipe[0], &read_result, sizeof(read_result)));
  close(result_pipe[0]);
  EXPECT_EQ(-1, read_result);
  ASSERT_EQ(reader, waitpid(reader, &status, 0));
  EXPECT_EQ(0, status);

  EXPECT_EQ(-1, setpgid(-1, 1));
  EXPECT_EQ(-1, getpgid(999999));
  EXPECT_EQ(-1, procctl(-1, JOBCTL_STOP));
  EXPECT_EQ(-1, procctl(getpgid(0), 999));

  printf("consolelinetest: jobctl OK\n");
  TEST_EXIT();
}

/**
 * 验证外部用户程序启动后 console 已恢复 cooked 行规约。
 *
 * 程序先输出 ready，宿主机再输入 `ab<Backspace>c<Enter>`；内核应完成退格编辑
 * 后一次返回 `ac\n`。传入 `jobctl` 时改为执行无需宿主机输入的作业控制断言。
 *
 * @return 通过 exit status 返回；成功为 0，读取错误或断言失败为 1。
 */
int
main(int argc, char *argv[])
{
  char buffer[8] = {0};
  int count;

  if(argc == 2 && strcmp(argv[1], "jobctl") == 0){
    run_jobctl_test();
    exit(1);
  }
  if(argc != 1){
    fprintf(2, "Usage: consolelinetest [jobctl]\n");
    exit(2);
  }

  printf("consolelinetest: ready\n");
  count = read(0, buffer, sizeof(buffer));
  if(count != 3 || buffer[0] != 'a' || buffer[1] != 'c' || buffer[2] != '\n'){
    printf("consolelinetest: count=%d bytes=%d,%d,%d\n",
           count, buffer[0], buffer[1], buffer[2]);
    exit(1);
  }
  printf("consolelinetest: OK\n");
  exit(0);
}
