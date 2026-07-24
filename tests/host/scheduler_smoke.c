#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TIMEOUT_SECONDS 180
#define READ_CHUNK_SIZE 4096

/** 登录 Shell 根目录下的完整 ANSI 提示符，用作 QEMU 输入同步边界。 */
static const char root_shell_prompt[] =
  "\033[1;32mroot@xv6\033[0m:\033[1;34m/\033[0m# ";

struct smoke_options {
  const char *policy;
  int cpus;
};

struct output_buffer {
  char *data;
  size_t length;
  size_t capacity;
};

/// 输出命令行用法。
///
/// @param program 当前程序名，仅用于错误提示。
static void
print_usage(const char *program)
{
  fprintf(stderr, "usage: %s --policy <rr|fifo|sjf|stcf|mlfq|cfs> --cpus <n>\n",
          program);
}

/// 判断策略名是否属于当前调度器支持的固定集合。
///
/// @param policy 待校验的策略名，不得为 NULL。
/// @return 支持时返回 true，否则返回 false。
static bool
is_supported_policy(const char *policy)
{
  static const char *policies[] = {"rr", "fifo", "sjf", "stcf", "mlfq", "cfs"};

  for(size_t i = 0; i < sizeof(policies) / sizeof(policies[0]); i++)
    if(strcmp(policy, policies[i]) == 0)
      return true;
  return false;
}

/// 解析宿主机 smoke runner 参数，并拒绝可能进入 make 变量的非法值。
///
/// @param argc 命令行参数数量。
/// @param argv 命令行参数数组，所有权归调用者。
/// @param options 输出解析后的策略与 CPU 数量，不接管字符串所有权。
/// @return 参数合法时返回 0，否则打印用法并返回 -1。
static int
parse_options(int argc, char **argv, struct smoke_options *options)
{
  options->policy = 0;
  options->cpus = 1;

  for(int i = 1; i < argc; i++){
    if(strcmp(argv[i], "--policy") == 0 && i + 1 < argc){
      options->policy = argv[++i];
    } else if(strcmp(argv[i], "--cpus") == 0 && i + 1 < argc){
      char *end = 0;
      long value = strtol(argv[++i], &end, 10);
      if(end == argv[i] || *end != '\0' || value < 1 || value > 64)
        return -1;
      options->cpus = (int)value;
    } else {
      return -1;
    }
  }

  if(options->policy == 0 || !is_supported_policy(options->policy))
    return -1;
  return 0;
}

/// 释放动态输出缓冲区。
///
/// @param output 待释放的缓冲区；允许其 data 为 NULL。
static void
free_output(struct output_buffer *output)
{
  free(output->data);
  output->data = 0;
  output->length = 0;
  output->capacity = 0;
}

/// 将 QEMU 新输出追加到以 NUL 结尾的动态缓冲区。
///
/// @param output 保存完整控制台输出的缓冲区。
/// @param data 本次读取的数据，函数只复制、不接管所有权。
/// @param size 本次数据字节数。
/// @return 追加成功返回 0；内存分配失败返回 -1，原缓冲区仍可释放。
static int
append_output(struct output_buffer *output, const char *data, size_t size)
{
  size_t required = output->length + size + 1;
  if(required > output->capacity){
    size_t capacity = output->capacity == 0 ? READ_CHUNK_SIZE : output->capacity;
    while(capacity < required)
      capacity *= 2;
    char *expanded = realloc(output->data, capacity);
    if(expanded == 0)
      return -1;
    output->data = expanded;
    output->capacity = capacity;
  }

  memcpy(output->data + output->length, data, size);
  output->length += size;
  output->data[output->length] = '\0';
  return 0;
}

/// 可靠地向 QEMU 标准输入写入一段控制字节或命令。
///
/// @param fd QEMU 标准输入管道写端。
/// @param data 待写入字节序列，不接管所有权。
/// @param size 待写入字节数。
/// @return 全部写入返回 0；管道关闭或系统调用失败返回 -1。
static int
write_all(int fd, const void *data, size_t size)
{
  const char *cursor = data;
  size_t written = 0;

  while(written < size){
    ssize_t count = write(fd, cursor + written, size - written);
    if(count < 0 && errno == EINTR)
      continue;
    if(count <= 0)
      return -1;
    written += (size_t)count;
  }
  return 0;
}

/// 返回单调时钟秒数，用于限制整个 QEMU 会话时长。
///
/// @return 当前 CLOCK_MONOTONIC 秒数；读取失败时返回 -1。
static time_t
monotonic_seconds(void)
{
  struct timespec now;
  if(clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    return (time_t)-1;
  return now.tv_sec;
}

/// 检查控制台中是否出现内核致命错误标记。
///
/// @param output 已累积且以 NUL 结尾的控制台输出；允许为 NULL。
/// @return 出现 panic 或 kerneltrap 时返回 true。
static bool
contains_kernel_failure(const char *output)
{
  return output != 0 &&
         (strstr(output, "panic:") != 0 || strstr(output, "kerneltrap") != 0);
}

/// 启动 make qemu，并把标准输入、标准输出和标准错误接入父进程管道。
///
/// 子进程会成为独立进程组组长，失败清理时可同时终止 make 与 QEMU。
///
/// @param options 已校验的策略与 CPU 数量。
/// @param input_fd 输出父进程写入 QEMU 标准输入的文件描述符。
/// @param output_fd 输出父进程读取 QEMU 控制台的文件描述符。
/// @return 成功返回子进程 PID；创建管道或 fork 失败返回 -1。
static pid_t
spawn_qemu(const struct smoke_options *options, int *input_fd, int *output_fd)
{
  int input_pipe[2];
  int output_pipe[2];

  if(pipe(input_pipe) < 0)
    return -1;
  if(pipe(output_pipe) < 0){
    close(input_pipe[0]);
    close(input_pipe[1]);
    return -1;
  }

  pid_t pid = fork();
  if(pid < 0){
    close(input_pipe[0]);
    close(input_pipe[1]);
    close(output_pipe[0]);
    close(output_pipe[1]);
    return -1;
  }

  if(pid == 0){
    char policy_arg[64];
    char cpus_arg[32];

    if(setpgid(0, 0) < 0)
      _exit(127);
    if(dup2(input_pipe[0], STDIN_FILENO) < 0 ||
       dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
       dup2(output_pipe[1], STDERR_FILENO) < 0)
      _exit(127);

    close(input_pipe[0]);
    close(input_pipe[1]);
    close(output_pipe[0]);
    close(output_pipe[1]);

    snprintf(policy_arg, sizeof(policy_arg), "SCHED_POLICY=%s", options->policy);
    snprintf(cpus_arg, sizeof(cpus_arg), "CPUS=%d", options->cpus);
    execlp("make", "make", "-s", "--no-print-directory", "qemu",
           policy_arg, cpus_arg, "QEMUEXTRA=-snapshot", (char *)0);
    _exit(127);
  }

  // 父进程也尝试设置进程组，消除子进程 exec 前后的竞态。
  if(setpgid(pid, pid) < 0 && errno != EACCES && errno != ESRCH){
    kill(pid, SIGKILL);
    close(input_pipe[0]);
    close(input_pipe[1]);
    close(output_pipe[0]);
    close(output_pipe[1]);
    waitpid(pid, 0, 0);
    return -1;
  }

  close(input_pipe[0]);
  close(output_pipe[1]);
  *input_fd = input_pipe[1];
  *output_fd = output_pipe[0];
  return pid;
}

/// 终止 runner 创建的整个进程组，并回收直接子进程。
///
/// @param pid make 进程 PID，同时也是进程组 ID。
static void
terminate_process_group(pid_t pid)
{
  int status;

  if(pid <= 0)
    return;
  kill(-pid, SIGTERM);
  for(int i = 0; i < 10; i++){
    pid_t result = waitpid(pid, &status, WNOHANG);
    if(result == pid || (result < 0 && errno == ECHILD))
      return;
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
    while(nanosleep(&delay, &delay) < 0 && errno == EINTR)
      ;
  }
  kill(-pid, SIGKILL);
  while(waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
}

/// 驱动一次完整调度器 smoke 会话并校验可观察结果。
///
/// @param options 已校验的策略与 CPU 数量；单核运行 verify，多核运行 smoke。
/// @return 所有断言满足且 QEMU 正常退出时返回 0，否则返回 1。
static int
run_smoke(const struct smoke_options *options)
{
  static const char qemu_quit[] = {1, 'x'};
  struct output_buffer output = {0};
  char expected_banner[64];
  char expected_result[64];
  char command[32];
  int input_fd = -1;
  int output_fd = -1;
  pid_t pid = spawn_qemu(options, &input_fd, &output_fd);
  if(pid < 0){
    perror("scheduler_smoke: spawn qemu");
    return 1;
  }

  snprintf(expected_banner, sizeof(expected_banner), "scheduler: %s", options->policy);
  snprintf(expected_result, sizeof(expected_result), "schedtest: OK policy=%s", options->policy);
  snprintf(command, sizeof(command), "schedtest %s\n",
           options->cpus == 1 ? "verify" : "smoke");

  bool command_sent = false;
  bool quit_sent = false;
  size_t command_output_offset = 0;
  time_t started = monotonic_seconds();
  int result = 1;

  for(;;){
    time_t now = monotonic_seconds();
    if(started == (time_t)-1 || now == (time_t)-1 ||
       now - started >= DEFAULT_TIMEOUT_SECONDS){
      fprintf(stderr, "scheduler_smoke: timed out after %d seconds\n",
              DEFAULT_TIMEOUT_SECONDS);
      break;
    }

    struct pollfd pollfd = {
      .fd = output_fd,
      .events = POLLIN | POLLHUP,
    };
    int ready = poll(&pollfd, 1, 1000);
    if(ready < 0){
      if(errno == EINTR)
        continue;
      perror("scheduler_smoke: poll");
      break;
    }

    if(ready > 0 && (pollfd.revents & (POLLIN | POLLHUP)) != 0){
      char chunk[READ_CHUNK_SIZE];
      ssize_t count = read(output_fd, chunk, sizeof(chunk));
      if(count < 0){
        if(errno == EINTR)
          continue;
        perror("scheduler_smoke: read");
        break;
      }
      if(count == 0){
        int status;
        pid_t exited;
        do {
          exited = waitpid(pid, &status, 0);
        } while(exited < 0 && errno == EINTR);
        if(exited == pid && quit_sent && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0)
          result = 0;
        else
          fprintf(stderr, "scheduler_smoke: qemu exited before validation completed\n");
        pid = -1;
        break;
      }

      if(write_all(STDOUT_FILENO, chunk, (size_t)count) < 0 ||
         append_output(&output, chunk, (size_t)count) < 0){
        perror("scheduler_smoke: record output");
        break;
      }
      if(contains_kernel_failure(output.data)){
        fprintf(stderr, "scheduler_smoke: kernel failure detected\n");
        break;
      }

      if(!command_sent && strstr(output.data, root_shell_prompt) != 0){
        if(strstr(output.data, expected_banner) == 0){
          fprintf(stderr, "scheduler_smoke: missing banner '%s'\n", expected_banner);
          break;
        }
        if(write_all(input_fd, command, strlen(command)) < 0){
          perror("scheduler_smoke: send command");
          break;
        }
        command_sent = true;
        command_output_offset = output.length;
      }

      if(command_sent && !quit_sent && output.length > command_output_offset){
        char *test_output = output.data + command_output_offset;
        char *success = strstr(test_output, expected_result);
        if(success != 0 && strstr(success, root_shell_prompt) != 0){
          if(write_all(input_fd, qemu_quit, sizeof(qemu_quit)) < 0){
            perror("scheduler_smoke: quit qemu");
            break;
          }
          quit_sent = true;
          close(input_fd);
          input_fd = -1;
        }
      }
    }

    int status;
    pid_t exited = waitpid(pid, &status, WNOHANG);
    if(exited == pid){
      pid = -1;
      if(quit_sent && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        result = 0;
      else
        fprintf(stderr, "scheduler_smoke: qemu exited before validation completed\n");
      break;
    }
    if(exited < 0 && errno != EINTR){
      perror("scheduler_smoke: waitpid");
      pid = -1;
      break;
    }
  }

  if(input_fd >= 0)
    close(input_fd);
  close(output_fd);
  if(pid > 0)
    terminate_process_group(pid);
  free_output(&output);
  return result;
}

/// 解析参数并执行一次宿主机调度器 smoke 测试。
///
/// @param argc 命令行参数数量。
/// @param argv 命令行参数数组。
/// @return 成功返回 0；参数错误或测试失败返回非零值。
int
main(int argc, char **argv)
{
  struct smoke_options options;
  signal(SIGPIPE, SIG_IGN);
  if(parse_options(argc, argv, &options) < 0){
    print_usage(argv[0]);
    return 2;
  }
  return run_smoke(&options);
}
