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

#define TIMEOUT_SECONDS 180
#define READ_CHUNK_SIZE 4096
#define MAX_EVENTS 2048
#define MAX_SEGMENTS 2048
#define MAX_CPUS 64

/** 登录 Shell 根目录下的完整 ANSI 提示符，用作 QEMU 输入同步边界。 */
static const char root_shell_prompt[] =
  "\033[1;32mroot@xv6\033[0m:\033[1;34m/\033[0m# ";

struct options {
  const char *policy;
  const char *trace_path;
  const char *svg_path;
  const char *demo_args;
  int cpus;
};

struct output_buffer {
  char *data;
  size_t length;
  size_t capacity;
};

struct event {
  unsigned long seq;
  unsigned long ts;
  unsigned long tick;
  int cpu;
  int pid;
  char type[24];
  char reason[32];
  int runtime;
  int dispatches;
  int level;
  int weight;
  int vruntime;
  char name[32];
};

struct trace_data {
  char policy[16];
  int cpus;
  int expected_events;
  int event_count;
  int dropped;
  int done;
  struct event event[MAX_EVENTS];
};

struct segment {
  unsigned long start_ts;
  unsigned long end_ts;
  int cpu;
  int pid;
  char reason[32];
};

/** 输出 host SVG 生成器的命令行参数。 */
static void
usage(const char *program)
{
  fprintf(stderr,
          "usage: %s --policy <rr|fifo|sjf|stcf|mlfq|cfs> --cpus <n> --trace <path> --svg <path> [--demo-args <args>]\n",
          program);
}

/**
 * parse_options 解析并校验传给 make/qemu 的参数。
 *
 * @param argc 参数数量。
 * @param argv 参数数组，所有权归调用者。
 * @param options 输出解析结果。
 * @return 参数合法返回 0，否则返回 -1。
 */
static int
parse_options(int argc, char **argv, struct options *options)
{
  memset(options, 0, sizeof(*options));
  options->cpus = 1;
  for(int i = 1; i < argc; i++){
    if(strcmp(argv[i], "--policy") == 0 && i + 1 < argc){
      options->policy = argv[++i];
    } else if(strcmp(argv[i], "--cpus") == 0 && i + 1 < argc){
      char *end = 0;
      long value = strtol(argv[++i], &end, 10);
      if(end == argv[i] || *end != '\0' || value < 1 || value > MAX_CPUS)
        return -1;
      options->cpus = (int)value;
    } else if(strcmp(argv[i], "--trace") == 0 && i + 1 < argc){
      options->trace_path = argv[++i];
    } else if(strcmp(argv[i], "--svg") == 0 && i + 1 < argc){
      options->svg_path = argv[++i];
    } else if(strcmp(argv[i], "--demo-args") == 0 && i + 1 < argc){
      options->demo_args = argv[++i];
    } else {
      return -1;
    }
  }
  return options->policy && options->trace_path && options->svg_path ? 0 : -1;
}

/**
 * append_output 将 QEMU 输出追加到 NUL 结尾动态缓冲。
 *
 * @param output 累积输出缓冲。
 * @param data 本次读取内容。
 * @param size 本次数据字节数。
 * @return 成功返回 0，内存分配失败返回 -1。
 */
static int
append_output(struct output_buffer *output, const char *data, size_t size)
{
  size_t required = output->length + size + 1;
  if(required > output->capacity){
    size_t capacity = output->capacity ? output->capacity : READ_CHUNK_SIZE;
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

/**
 * write_all 可靠写入一段控制命令。
 *
 * @param fd 目标文件描述符。
 * @param data 待写入字节。
 * @param size 字节数。
 * @return 全部写入返回 0，失败返回 -1。
 */
static int
write_all(int fd, const void *data, size_t size)
{
  const char *cursor = data;
  size_t written = 0;
  while(written < size){
    ssize_t n = write(fd, cursor + written, size - written);
    if(n < 0 && errno == EINTR)
      continue;
    if(n <= 0)
      return -1;
    written += (size_t)n;
  }
  return 0;
}

/** @return 当前 CLOCK_MONOTONIC 秒数；失败返回 -1。 */
static time_t
monotonic_seconds(void)
{
  struct timespec now;
  if(clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    return (time_t)-1;
  return now.tv_sec;
}

/**
 * spawn_qemu 通过 make qemu 启动 snapshot 会话。
 *
 * @param options 已校验参数。
 * @param input_fd 输出 QEMU stdin 写端。
 * @param output_fd 输出 QEMU stdout/stderr 读端。
 * @return 子进程 PID；失败返回 -1。
 */
static pid_t
spawn_qemu(const struct options *options, int *input_fd, int *output_fd)
{
  int inpipe[2];
  int outpipe[2];
  if(pipe(inpipe) < 0 || pipe(outpipe) < 0)
    return -1;
  pid_t pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    char policy_arg[64];
    char cpus_arg[32];
    if(setpgid(0, 0) < 0)
      _exit(127);
    dup2(inpipe[0], STDIN_FILENO);
    dup2(outpipe[1], STDOUT_FILENO);
    dup2(outpipe[1], STDERR_FILENO);
    close(inpipe[0]);
    close(inpipe[1]);
    close(outpipe[0]);
    close(outpipe[1]);
    snprintf(policy_arg, sizeof(policy_arg), "SCHED_POLICY=%s", options->policy);
    snprintf(cpus_arg, sizeof(cpus_arg), "CPUS=%d", options->cpus);
    execlp("make", "make", "-s", "--no-print-directory", "qemu",
           policy_arg, cpus_arg, "QEMUEXTRA=-snapshot", (char *)0);
    _exit(127);
  }
  if(setpgid(pid, pid) < 0 && errno != EACCES && errno != ESRCH)
    return -1;
  close(inpipe[0]);
  close(outpipe[1]);
  *input_fd = inpipe[1];
  *output_fd = outpipe[0];
  return pid;
}

/**
 * terminate_group 终止 make 和其下的 QEMU 进程组。
 *
 * @param pid 进程组 ID。
 */
static void
terminate_group(pid_t pid)
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

/**
 * save_trace_file 从完整控制台中截取 SCHEDTRACE 协议并保存。
 *
 * @param output QEMU 完整输出。
 * @param path 输出 trace 文件路径。
 * @return 成功返回 0，缺少协议或写入失败返回 -1。
 */
static int
save_trace_file(const char *output, const char *path)
{
  char *start = strstr(output, "SCHEDTRACE version=");
  char *done = strstr(output, "SCHEDTRACE done status=0");
  if(start == 0 || done == 0)
    return -1;
  done += strlen("SCHEDTRACE done status=0");
  FILE *file = fopen(path, "w");
  if(file == 0)
    return -1;
  fwrite(start, 1, (size_t)(done - start), file);
  fputc('\n', file);
  int failed = ferror(file);
  fclose(file);
  return failed ? -1 : 0;
}

/**
 * parse_trace 解析 schedviz dump 的稳定文本协议。
 *
 * @param path trace 文件路径。
 * @param trace 输出结构化事件。
 * @return 协议完整且 dropped 为 0 时返回 0，否则返回 -1。
 */
static int
parse_trace(const char *path, struct trace_data *trace)
{
  char line[512];
  FILE *file = fopen(path, "r");
  if(file == 0)
    return -1;
  memset(trace, 0, sizeof(*trace));
  while(fgets(line, sizeof(line), file)){
    if(strncmp(line, "SCHEDTRACE version=", 19) == 0){
      if(sscanf(line, "SCHEDTRACE version=1 policy=%15s cpus=%d events=%d dropped=%d",
                trace->policy, &trace->cpus, &trace->expected_events,
                &trace->dropped) != 4){
        fclose(file);
        return -1;
      }
    } else if(strncmp(line, "SCHEDTRACE event", 16) == 0){
      struct event *event;
      if(trace->event_count >= MAX_EVENTS){
        fclose(file);
        return -1;
      }
      event = &trace->event[trace->event_count++];
      if(sscanf(line,
                "SCHEDTRACE event seq=%lu ts=%lu tick=%lu cpu=%d pid=%d type=%23s reason=%31s state=%*d slice=%*d runtime=%d dispatches=%d remaining=%*d level=%d epoch=%*d weight=%d vruntime=%d name=%31s",
                &event->seq, &event->ts, &event->tick, &event->cpu,
                &event->pid, event->type, event->reason, &event->runtime,
                &event->dispatches, &event->level, &event->weight,
                &event->vruntime, event->name) != 13){
        fclose(file);
        return -1;
      }
    } else if(strcmp(line, "SCHEDTRACE done status=0\n") == 0){
      trace->done = 1;
    }
  }
  fclose(file);
  if(!trace->done || trace->dropped != 0)
    return -1;
  return trace->event_count == trace->expected_events ? 0 : -1;
}

/**
 * collect_segments 将事件配对为 SVG 矩形区间。
 *
 * @param trace 已解析事件。
 * @param segments 输出区间。
 * @return 区间数量。
 */
static int
collect_segments(const struct trace_data *trace, struct segment segments[])
{
  struct event starts[MAX_CPUS];
  bool has_start[MAX_CPUS] = {0};
  int n = 0;
  for(int i = 0; i < trace->event_count; i++){
    const struct event *event = &trace->event[i];
    if(event->cpu < 0 || event->cpu >= MAX_CPUS)
      continue;
    if(strcmp(event->type, "RUN_START") == 0){
      starts[event->cpu] = *event;
      has_start[event->cpu] = true;
    } else if(strcmp(event->type, "RUN_STOP") == 0 && has_start[event->cpu] &&
              n < MAX_SEGMENTS){
      segments[n].start_ts = starts[event->cpu].ts;
      segments[n].end_ts = event->ts > starts[event->cpu].ts ?
                           event->ts : starts[event->cpu].ts + 1;
      segments[n].cpu = event->cpu;
      segments[n].pid = event->pid;
      snprintf(segments[n].reason, sizeof(segments[n].reason), "%s", event->reason);
      n++;
      has_start[event->cpu] = false;
    }
  }
  return n;
}

/** @return PID 对应的稳定填充颜色。 */
static const char *
color_for_pid(int pid)
{
  static const char *colors[] = {
    "#4c78a8", "#f58518", "#54a24b", "#e45756", "#72b7b2", "#b279a2"
  };
  if(pid < 0)
    pid = -pid;
  return colors[pid % (int)(sizeof(colors) / sizeof(colors[0]))];
}

/**
 * write_svg 直接输出包含 CPU 泳道和运行矩形的 SVG XML。
 *
 * @param path SVG 输出路径。
 * @param trace 已解析 trace。
 * @return 写入成功且包含至少一个矩形时返回 0，否则返回 -1。
 */
static int
write_svg(const char *path, const struct trace_data *trace)
{
  struct segment segments[MAX_SEGMENTS];
  int segment_count = collect_segments(trace, segments);
  if(segment_count <= 0)
    return -1;
  unsigned long min_ts = segments[0].start_ts;
  unsigned long max_ts = segments[0].end_ts;
  for(int i = 0; i < segment_count; i++){
    if(segments[i].start_ts < min_ts)
      min_ts = segments[i].start_ts;
    if(segments[i].end_ts > max_ts)
      max_ts = segments[i].end_ts;
  }
  if(max_ts <= min_ts)
    max_ts = min_ts + 1;
  int width = 960;
  int lane_h = 44;
  int height = 100 + trace->cpus * lane_h + 80;
  FILE *file = fopen(path, "w");
  if(file == 0)
    return -1;
  fprintf(file, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n",
          width, height, width, height);
  fprintf(file, "<title>schedviz %s cpus=%d dropped=%d</title>\n",
          trace->policy, trace->cpus, trace->dropped);
  fprintf(file, "<rect width=\"100%%\" height=\"100%%\" fill=\"#ffffff\"/>\n");
  fprintf(file, "<text x=\"24\" y=\"32\" font-family=\"monospace\" font-size=\"18\">schedviz policy=%s cpus=%d</text>\n",
          trace->policy, trace->cpus);
  for(int cpu = 0; cpu < trace->cpus; cpu++){
    int y = 64 + cpu * lane_h;
    fprintf(file, "<text x=\"24\" y=\"%d\" font-family=\"monospace\" font-size=\"13\">CPU%d</text>\n", y + 20, cpu);
    fprintf(file, "<rect x=\"80\" y=\"%d\" width=\"840\" height=\"28\" fill=\"#f5f5f5\" stroke=\"#cccccc\"/>\n", y);
  }
  for(int i = 0; i < segment_count; i++){
    int x = 80 + (int)((segments[i].start_ts - min_ts) * 840 / (max_ts - min_ts));
    int x2 = 80 + (int)((segments[i].end_ts - min_ts) * 840 / (max_ts - min_ts));
    if(x2 <= x)
      x2 = x + 1;
    int y = 64 + segments[i].cpu * lane_h;
    fprintf(file,
            "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"28\" fill=\"%s\"><title>pid=%d cpu=%d reason=%s start=%lu duration=%lu</title></rect>\n",
            x, y, x2 - x, color_for_pid(segments[i].pid), segments[i].pid,
            segments[i].cpu, segments[i].reason, segments[i].start_ts,
            segments[i].end_ts - segments[i].start_ts);
    fprintf(file, "<text x=\"%d\" y=\"%d\" font-family=\"monospace\" font-size=\"10\" fill=\"#111\">%d</text>\n",
            x + 3, y + 18, segments[i].pid);
  }
  fprintf(file, "<text x=\"80\" y=\"%d\" font-family=\"monospace\" font-size=\"12\">time axis: normalized r_time(), dropped=%d, rectangles=%d</text>\n",
          height - 28, trace->dropped, segment_count);
  fprintf(file, "</svg>\n");
  int failed = ferror(file);
  fclose(file);
  return failed ? -1 : 0;
}

/**
 * run_session 启动 QEMU，执行 schedviz，并生成 trace/SVG 文件。
 *
 * @param options 已校验命令行参数。
 * @return 完整成功返回 0；超时、panic、协议错误或写文件失败返回 1。
 */
static int
run_session(const struct options *options)
{
  static const char quit[] = {1, 'x'};
  char command[256];
  struct output_buffer output = {0};
  int input_fd = -1;
  int output_fd = -1;
  pid_t pid = spawn_qemu(options, &input_fd, &output_fd);
  bool command_sent = false;
  bool quit_sent = false;
  int result = 1;
  time_t started = monotonic_seconds();
  if(pid < 0)
    return 1;
  // QEMU shell 命令必须一次写入，过长参数直接失败，避免执行被截断的实验。
  if(options->demo_args && options->demo_args[0]){
    if(snprintf(command, sizeof(command), "schedviz demo %s\nschedviz dump\n",
                options->demo_args) >= (int)sizeof(command))
      return 1;
  } else {
    snprintf(command, sizeof(command), "schedviz demo --plain\nschedviz dump\n");
  }
  for(;;){
    time_t now = monotonic_seconds();
    if(now == (time_t)-1 || started == (time_t)-1 ||
       now - started > TIMEOUT_SECONDS)
      break;
    struct pollfd pollfd = {.fd = output_fd, .events = POLLIN | POLLHUP};
    int ready = poll(&pollfd, 1, 1000);
    if(ready < 0 && errno == EINTR)
      continue;
    if(ready < 0)
      break;
    if(ready > 0 && (pollfd.revents & (POLLIN | POLLHUP))){
      char chunk[READ_CHUNK_SIZE];
      ssize_t n = read(output_fd, chunk, sizeof(chunk));
      if(n < 0 && errno == EINTR)
        continue;
      if(n <= 0){
        int status;
        if(quit_sent){
          while(waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
          pid = -1;
          result = 0;
        }
        break;
      }
      if(write_all(STDOUT_FILENO, chunk, (size_t)n) < 0 ||
         append_output(&output, chunk, (size_t)n) < 0)
        break;
      if(strstr(output.data, "panic:") || strstr(output.data, "kerneltrap"))
        break;
      if(!command_sent && strstr(output.data, root_shell_prompt)){
        if(write_all(input_fd, command, strlen(command)) < 0)
          break;
        command_sent = true;
      }
      if(command_sent && !quit_sent &&
         strstr(output.data, "SCHEDTRACE done status=0")){
        if(write_all(input_fd, quit, sizeof(quit)) < 0)
          break;
        quit_sent = true;
        close(input_fd);
        input_fd = -1;
      }
    }
    int status;
    pid_t exited = waitpid(pid, &status, WNOHANG);
    if(exited == pid){
      pid = -1;
      if(quit_sent && ((WIFEXITED(status) && WEXITSTATUS(status) == 0) ||
                       WIFSIGNALED(status)))
        result = 0;
      break;
    }
  }
  if(input_fd >= 0)
    close(input_fd);
  close(output_fd);
  if(pid > 0)
    terminate_group(pid);
  if(result == 0 && save_trace_file(output.data, options->trace_path) == 0){
    struct trace_data trace;
    if(parse_trace(options->trace_path, &trace) == 0 &&
       write_svg(options->svg_path, &trace) == 0)
      result = 0;
    else
      result = 1;
  } else {
    result = 1;
  }
  free(output.data);
  if(result == 0)
    printf("scheduler_visualizer: wrote %s and %s\n",
           options->trace_path, options->svg_path);
  return result;
}

/**
 * main 解析参数并执行宿主机 SVG 生成器。
 *
 * @param argc 参数数量。
 * @param argv 参数数组。
 * @return 成功返回 0，参数错误返回 2，执行失败返回 1。
 */
int
main(int argc, char **argv)
{
  struct options options;
  signal(SIGPIPE, SIG_IGN);
  if(parse_options(argc, argv, &options) < 0){
    usage(argv[0]);
    return 2;
  }
  return run_session(&options);
}
