#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "kernel/schedstat.h"
#include "kernel/schedtrace_abi.h"
#include "user/user.h"

#define MAX_WORKERS SCHEDTRACE_MAX_FILTERS
#define CHART_WIDTH 64
// 长场景把用户指定的 seconds 映射为总 CPU tick，保证实验能稳定结束。
#define TICKS_PER_SECOND 1

#ifndef XV6_CPUS
#define XV6_CPUS NCPU
#endif

struct demo_options {
  int plain;
  int seconds;
  int workers;
};

struct worker_spec {
  int id;
  int hint;
  int weight;
  int runtime_ticks;
  int wall_ticks;
};

struct worker_info {
  int id;
  int pid;
  int hint;
  int weight;
  char glyph;
};

struct run_segment {
  int cpu;
  int pid;
  unsigned long start_tick;
  unsigned long end_tick;
  int reason;
};

static struct schedtrace_snapshot snapshot;
static struct run_segment chart_segments[SCHEDTRACE_MAX_EVENTS / 2];

/**
 * usage 输出 schedviz 支持的稳定命令接口。
 */
static void
usage(void)
{
  fprintf(2, "usage: schedviz demo [--plain] [--seconds n] [--workers n] | schedviz dump\n");
}

/**
 * parse_positive_int 解析正整数命令行参数。
 *
 * @param text 参数文本，只接受十进制数字。
 * @param value 输出解析后的正整数。
 * @return 参数合法返回 0；为空、包含非数字或小于 1 时返回 -1。
 */
static int
parse_positive_int(char *text, int *value)
{
  int result = 0;

  if(text == 0 || text[0] == 0)
    return -1;
  for(int i = 0; text[i]; i++){
    if(text[i] < '0' || text[i] > '9')
      return -1;
    result = result * 10 + text[i] - '0';
  }
  if(result < 1)
    return -1;
  *value = result;
  return 0;
}

/**
 * parse_demo_options 解析 demo 子命令的可选实验参数。
 *
 * @param argc main 收到的参数数量。
 * @param argv main 收到的参数数组。
 * @param options 输出 demo 配置；seconds/workers 为 0 表示使用短默认场景。
 * @return 参数合法返回 0；未知参数或越界返回 -1。
 */
static int
parse_demo_options(int argc, char **argv, struct demo_options *options)
{
  memset(options, 0, sizeof(*options));
  for(int i = 2; i < argc; i++){
    if(strcmp(argv[i], "--plain") == 0){
      options->plain = 1;
    } else if(strcmp(argv[i], "--seconds") == 0 && i + 1 < argc){
      if(parse_positive_int(argv[++i], &options->seconds) < 0)
        return -1;
    } else if(strcmp(argv[i], "--workers") == 0 && i + 1 < argc){
      if(parse_positive_int(argv[++i], &options->workers) < 0)
        return -1;
    } else {
      return -1;
    }
  }
  if(options->workers > MAX_WORKERS)
    return -1;
  if(options->seconds > 0 && options->workers == 0)
    options->workers = 8;
  if(options->workers > 0 && options->seconds == 0)
    options->seconds = 30;
  return 0;
}

/**
 * policy_name 将 SCHED_POLICY_* 转为协议和界面共用的短名称。
 *
 * @param policy sched_get_stats() 返回的策略编号。
 * @return 静态策略名；未知编号返回 unknown。
 */
static char *
policy_name(int policy)
{
  static char *names[] = {"rr", "fifo", "sjf", "stcf", "mlfq", "cfs"};
  if(policy < 0 || policy >= 6)
    return "unknown";
  return names[policy];
}

/**
 * event_type_name 返回 dump 协议中的事件类型字符串。
 *
 * @param type SCHEDTRACE_EVENT_* 编号。
 * @return 稳定协议字符串；未知值返回 UNKNOWN。
 */
static char *
event_type_name(int type)
{
  switch(type){
  case SCHEDTRACE_EVENT_RUN_START:
    return "RUN_START";
  case SCHEDTRACE_EVENT_RUN_STOP:
    return "RUN_STOP";
  case SCHEDTRACE_EVENT_MLFQ_BOOST:
    return "MLFQ_BOOST";
  default:
    return "UNKNOWN";
  }
}

/**
 * reason_name 返回 RUN_STOP 的稳定停止原因字符串。
 *
 * @param reason SCHEDTRACE_REASON_* 编号。
 * @return dump、字符图和 SVG 共享的原因名称。
 */
static char *
reason_name(int reason)
{
  switch(reason){
  case SCHEDTRACE_REASON_NONE:
    return "NONE";
  case SCHEDTRACE_REASON_RR_QUANTUM:
    return "RR_QUANTUM";
  case SCHEDTRACE_REASON_STCF_SHORTER_TASK:
    return "STCF_SHORTER_TASK";
  case SCHEDTRACE_REASON_MLFQ_QUANTUM:
    return "MLFQ_QUANTUM";
  case SCHEDTRACE_REASON_MLFQ_HIGHER_QUEUE:
    return "MLFQ_HIGHER_QUEUE";
  case SCHEDTRACE_REASON_CFS_LOWER_VRUNTIME:
    return "CFS_LOWER_VRUNTIME";
  case SCHEDTRACE_REASON_VOLUNTARY_YIELD:
    return "VOLUNTARY_YIELD";
  case SCHEDTRACE_REASON_SLEEP:
    return "SLEEP";
  case SCHEDTRACE_REASON_EXIT:
    return "EXIT";
  case SCHEDTRACE_REASON_KILLED:
    return "KILLED";
  default:
    return "UNKNOWN";
  }
}

/**
 * read_exact 从 fd 读取固定字节数。
 *
 * @param fd 管道读端。
 * @param buffer 输出缓冲，函数会写满 size 字节。
 * @param size 需要读取的字节数。
 * @return 成功返回 0；EOF 或 read 失败返回 -1。
 */
static int
read_exact(int fd, void *buffer, int size)
{
  char *cursor = buffer;
  int total = 0;
  while(total < size){
    int n = read(fd, cursor + total, size - total);
    if(n <= 0)
      return -1;
    total += n;
  }
  return 0;
}

/**
 * write_exact 向 fd 写入固定字节数。
 *
 * @param fd 管道写端。
 * @param buffer 输入缓冲，函数只读取不接管所有权。
 * @param size 需要写入的字节数。
 * @return 成功返回 0；write 失败或管道关闭返回 -1。
 */
static int
write_exact(int fd, const void *buffer, int size)
{
  const char *cursor = buffer;
  int total = 0;
  while(total < size){
    int n = write(fd, cursor + total, size - total);
    if(n <= 0)
      return -1;
    total += n;
  }
  return 0;
}

/**
 * burn 执行一小段不可优化掉的 CPU 工作。
 */
static void
burn(void)
{
  volatile unsigned long value = 1;
  for(int i = 0; i < 100000; i++)
    value = value * 1664525 + 1013904223;
}

/**
 * consume_runtime_ticks 消耗当前进程统计中的调度 runtime tick。
 *
 * @param ticks 目标 runtime tick 数，必须为正。
 * @return 完成返回 0；读取调度统计失败返回 -1。
 */
static int
consume_runtime_ticks(int ticks)
{
  struct sched_stats start;
  struct sched_stats current;

  if(sched_get_stats(&start) < 0)
    return -1;
  do {
    burn();
    if(sched_get_stats(&current) < 0)
      return -1;
  } while(current.runtime_ticks - start.runtime_ticks < (unsigned long)ticks);
  return 0;
}

/**
 * worker_main 按教学场景参数运行一个 CPU worker。
 *
 * @param spec worker 的 hint、weight 和运行窗口配置。
 * @param readyfd worker 完成初始化后通知父进程的管道写端。
 * @param startfd 父进程释放 barrier 的管道读端。
 */
static void
worker_main(struct worker_spec spec, int readyfd, int startfd)
{
  char token = 'r';
  int deadline;

  if(sched_set_hint(spec.hint) < 0 || sched_set_weight(spec.weight) < 0)
    exit(1);
  if(write_exact(readyfd, &token, 1) < 0)
    exit(1);
  close(readyfd);
  if(read_exact(startfd, &token, 1) < 0)
    exit(1);
  close(startfd);

  if(spec.wall_ticks > 0){
    deadline = uptime() + spec.wall_ticks;
    while(uptime() < deadline)
      burn();
  } else if(consume_runtime_ticks(spec.runtime_ticks) < 0) {
    exit(1);
  }
  exit(0);
}

/**
 * current_snapshot 读取最近一次内核 schedtrace 快照。
 *
 * @return 成功返回 0；syscall 拒绝或 copyout 失败返回 -1。
 */
static int
current_snapshot(void)
{
  return schedtrace(SCHEDTRACE_OP_READ, &snapshot, SCHEDTRACE_MAX_EVENTS);
}

/**
 * register_workers 将已创建的 worker PID 加入 trace session 过滤器。
 *
 * @param workers worker 元数据数组。
 * @param count worker 数量。
 * @return 全部注册成功返回 0；任一 PID 无效返回 -1。
 */
static int
register_workers(struct worker_info workers[], int count)
{
  for(int i = 0; i < count; i++)
    if(schedtrace(SCHEDTRACE_OP_WATCH_PID, 0, workers[i].pid) < 0)
      return -1;
  return 0;
}

/**
 * run_standard_workers 运行同步释放的普通 worker 场景。
 *
 * @param specs 场景定义。
 * @param count worker 数量。
 * @param workers 输出 worker PID 与 legend 信息。
 * @return workload 完成且采样成功返回 0，否则返回 -1。
 */
static int
run_standard_workers(struct worker_spec specs[], int count,
                     struct worker_info workers[])
{
  int ready[2];
  int start[2];
  char token;

  if(pipe(ready) < 0 || pipe(start) < 0)
    return -1;

  for(int i = 0; i < count; i++){
    int pid = fork();
    if(pid < 0)
      return -1;
    if(pid == 0){
      close(ready[0]);
      close(start[1]);
      worker_main(specs[i], ready[1], start[0]);
    }
    workers[i].id = specs[i].id;
    workers[i].pid = pid;
    workers[i].hint = specs[i].hint;
    workers[i].weight = specs[i].weight;
    workers[i].glyph = 'A' + i;
  }

  close(ready[1]);
  close(start[0]);
  for(int i = 0; i < count; i++)
    if(read_exact(ready[0], &token, 1) < 0)
      return -1;

  schedtrace(SCHEDTRACE_OP_RESET, 0, 0);
  if(register_workers(workers, count) < 0)
    return -1;
  schedtrace(SCHEDTRACE_OP_START, 0, 0);
  for(int i = 0; i < count; i++)
    if(write_exact(start[1], "s", 1) < 0)
      return -1;
  close(start[1]);

  for(int i = 0; i < count; i++)
    wait(0);
  schedtrace(SCHEDTRACE_OP_STOP, 0, 0);
  close(ready[0]);
  return current_snapshot();
}

/**
 * run_stcf_workers 运行长任务先启动、短任务延迟唤醒的 STCF 场景。
 *
 * @param workers 输出两个 worker 的 PID 与 legend 信息。
 * @return workload 完成且采样成功返回 0，否则返回 -1。
 */
static int
run_stcf_workers(struct worker_info workers[])
{
  struct worker_spec specs[2] = {{0, 12, 1024, 12, 0}, {1, 2, 1024, 2, 0}};
  int ready[2];
  int start[2][2];
  char token;

  if(pipe(ready) < 0 || pipe(start[0]) < 0 || pipe(start[1]) < 0)
    return -1;
  for(int i = 0; i < 2; i++){
    int pid = fork();
    if(pid < 0)
      return -1;
    if(pid == 0){
      close(ready[0]);
      close(start[i][1]);
      worker_main(specs[i], ready[1], start[i][0]);
    }
    workers[i].id = i;
    workers[i].pid = pid;
    workers[i].hint = specs[i].hint;
    workers[i].weight = specs[i].weight;
    workers[i].glyph = 'A' + i;
  }

  close(ready[1]);
  close(start[0][0]);
  close(start[1][0]);
  if(read_exact(ready[0], &token, 1) < 0 ||
     read_exact(ready[0], &token, 1) < 0)
    return -1;

  schedtrace(SCHEDTRACE_OP_RESET, 0, 0);
  if(register_workers(workers, 2) < 0)
    return -1;
  if(sched_set_hint(1) < 0)
    return -1;
  schedtrace(SCHEDTRACE_OP_START, 0, 0);
  if(write_exact(start[0][1], "l", 1) < 0)
    return -1;
  sleep(2);
  if(write_exact(start[1][1], "s", 1) < 0)
    return -1;
  wait(0);
  wait(0);
  schedtrace(SCHEDTRACE_OP_STOP, 0, 0);
  close(ready[0]);
  close(start[0][1]);
  close(start[1][1]);
  return current_snapshot();
}

/**
 * fill_scaled_burst_specs 生成总运行量接近目标窗口的有限 CPU burst。
 *
 * @param specs 输出 worker 场景数组。
 * @param count worker 数量。
 * @param total_ticks 目标总 CPU tick，通常为 seconds * CPUS。
 */
static void
fill_scaled_burst_specs(struct worker_spec specs[], int count, int total_ticks)
{
  int pattern[] = {2, 5, 8, 13, 21, 3, 7, 11};
  int pattern_count = sizeof(pattern) / sizeof(pattern[0]);
  int sum = 0;

  for(int i = 0; i < count; i++)
    sum += pattern[i % pattern_count];
  if(total_ticks < count)
    total_ticks = count;
  for(int i = 0; i < count; i++){
    int ticks = pattern[i % pattern_count] * total_ticks / sum;
    if(ticks < 1)
      ticks = 1;
    specs[i].id = i;
    specs[i].hint = ticks;
    specs[i].weight = 1024;
    specs[i].runtime_ticks = ticks;
    specs[i].wall_ticks = 0;
  }
}

/**
 * apply_cfs_weight_pattern 为 CFS 长场景设置一组可观察的权重梯度。
 *
 * @param specs 输出 worker 场景数组。
 * @param count worker 数量。
 */
static void
apply_cfs_weight_pattern(struct worker_spec specs[], int count)
{
  int weights[] = {2048, 1024, 512, 1536, 768, 256, 3072, 1280};
  int weight_count = sizeof(weights) / sizeof(weights[0]);

  for(int i = 0; i < count; i++)
    specs[i].weight = weights[i % weight_count];
}

/**
 * run_scaled_stcf_workers 运行长任务先到、多个短任务延迟到达的 STCF 长场景。
 *
 * @param count worker 数量，至少 2。
 * @param seconds 目标观察秒数。
 * @param workers 输出 worker PID 与 legend 信息。
 * @return workload 完成且采样成功返回 0，否则返回 -1。
 */
static int
run_scaled_stcf_workers(int count, int seconds, struct worker_info workers[])
{
  struct worker_spec specs[MAX_WORKERS];
  int ready[2];
  int long_start[2];
  int short_start[2];
  char token;
  int total_ticks = seconds * TICKS_PER_SECOND;

  if(count < 2)
    count = 2;
  specs[0].id = 0;
  specs[0].hint = total_ticks;
  specs[0].weight = 1024;
  specs[0].runtime_ticks = total_ticks;
  specs[0].wall_ticks = 0;
  for(int i = 1; i < count; i++){
    specs[i].id = i;
    specs[i].hint = 2 + (i % 3);
    specs[i].weight = 1024;
    specs[i].runtime_ticks = specs[i].hint;
    specs[i].wall_ticks = 0;
  }

  if(pipe(ready) < 0 || pipe(long_start) < 0 || pipe(short_start) < 0)
    return -1;

  for(int i = 0; i < count; i++){
    int pid = fork();
    if(pid < 0)
      return -1;
    if(pid == 0){
      close(ready[0]);
      close(long_start[1]);
      close(short_start[1]);
      if(i == 0){
        close(short_start[0]);
        worker_main(specs[i], ready[1], long_start[0]);
      } else {
        close(long_start[0]);
        worker_main(specs[i], ready[1], short_start[0]);
      }
    }
    workers[i].id = i;
    workers[i].pid = pid;
    workers[i].hint = specs[i].hint;
    workers[i].weight = specs[i].weight;
    workers[i].glyph = 'A' + i;
  }

  close(ready[1]);
  close(long_start[0]);
  close(short_start[0]);
  for(int i = 0; i < count; i++)
    if(read_exact(ready[0], &token, 1) < 0)
      return -1;

  schedtrace(SCHEDTRACE_OP_RESET, 0, 0);
  if(register_workers(workers, count) < 0)
    return -1;
  if(sched_set_hint(1) < 0)
    return -1;
  schedtrace(SCHEDTRACE_OP_START, 0, 0);
  if(write_exact(long_start[1], "l", 1) < 0)
    return -1;
  close(long_start[1]);
  sleep(2);
  for(int i = 1; i < count; i++)
    if(write_exact(short_start[1], "s", 1) < 0)
      return -1;
  close(short_start[1]);

  for(int i = 0; i < count; i++)
    wait(0);
  schedtrace(SCHEDTRACE_OP_STOP, 0, 0);
  close(ready[0]);
  return current_snapshot();
}

/**
 * glyph_for_pid 查找 worker PID 对应的稳定单字符标识。
 *
 * @param workers worker 元数据。
 * @param count worker 数量。
 * @param pid 待查找的进程号。
 * @return 匹配时返回 A/B/C...；未知 PID 返回 '?'。
 */
static char
glyph_for_pid(struct worker_info workers[], int count, int pid)
{
  for(int i = 0; i < count; i++)
    if(workers[i].pid == pid)
      return workers[i].glyph;
  return '?';
}

/**
 * collect_segments 将 RUN_START/RUN_STOP 事件配对为运行区间。
 *
 * @param segments 输出区间数组。
 * @param max_segments 输出容量。
 * @return 实际区间数量。
 */
static int
collect_segments(struct run_segment segments[], int max_segments)
{
  struct schedtrace_event starts[NCPU];
  int has_start[NCPU];
  int n = 0;

  memset(has_start, 0, sizeof(has_start));
  for(int i = 0; i < snapshot.events; i++){
    struct schedtrace_event *event = &snapshot.events_buffer[i];
    if(event->cpu_id < 0 || event->cpu_id >= NCPU)
      continue;
    if(event->event_type == SCHEDTRACE_EVENT_RUN_START){
      starts[event->cpu_id] = *event;
      has_start[event->cpu_id] = 1;
    } else if(event->event_type == SCHEDTRACE_EVENT_RUN_STOP &&
              has_start[event->cpu_id] && n < max_segments){
      segments[n].cpu = event->cpu_id;
      segments[n].pid = event->pid;
      segments[n].start_tick = starts[event->cpu_id].scheduler_clock;
      segments[n].end_tick = event->scheduler_clock;
      if(segments[n].end_tick <= segments[n].start_tick)
        segments[n].end_tick = segments[n].start_tick + 1;
      segments[n].reason = event->stop_reason;
      n++;
      has_start[event->cpu_id] = 0;
    }
  }
  return n;
}

/**
 * print_axis 打印固定宽度字符图的时间刻度。
 *
 * @param min_tick 归一化起点。
 * @param max_tick 归一化终点。
 */
static void
print_axis(unsigned long min_tick, unsigned long max_tick)
{
  printf("time ");
  for(int i = 0; i < CHART_WIDTH; i++){
    if(i == 0)
      printf("%d", 0);
    else if(i % 10 == 0)
      printf("%d", (int)(min_tick + (max_tick - min_tick) * i / CHART_WIDTH));
    else
      printf(" ");
  }
  printf("\n");
}

/**
 * print_chart 输出每 CPU 一条泳道的 CPU 时间片字符图。
 *
 * @param workers worker 元数据。
 * @param count worker 数量。
 */
static void
print_chart(struct worker_info workers[], int count)
{
  struct run_segment *segments = chart_segments;
  char lanes[NCPU][CHART_WIDTH + 1];
  unsigned long min_tick = 0;
  unsigned long max_tick = 1;
  int segment_count = collect_segments(segments, SCHEDTRACE_MAX_EVENTS / 2);

  if(segment_count > 0){
    min_tick = segments[0].start_tick;
    max_tick = segments[0].end_tick;
    for(int i = 0; i < segment_count; i++){
      if(segments[i].start_tick < min_tick)
        min_tick = segments[i].start_tick;
      if(segments[i].end_tick > max_tick)
        max_tick = segments[i].end_tick;
    }
    if(max_tick <= min_tick)
      max_tick = min_tick + 1;
  }

  for(int cpu = 0; cpu < NCPU; cpu++){
    for(int col = 0; col < CHART_WIDTH; col++)
      lanes[cpu][col] = '.';
    lanes[cpu][CHART_WIDTH] = 0;
  }

  for(int i = 0; i < segment_count; i++){
    int start = (int)((segments[i].start_tick - min_tick) * CHART_WIDTH /
                      (max_tick - min_tick));
    int end = (int)((segments[i].end_tick - min_tick) * CHART_WIDTH /
                    (max_tick - min_tick));
    if(end <= start)
      end = start + 1;
    if(start < 0)
      start = 0;
    if(end > CHART_WIDTH)
      end = CHART_WIDTH;
    for(int col = start; col < end; col++)
      lanes[segments[i].cpu][col] = glyph_for_pid(workers, count, segments[i].pid);
  }

  printf("schedviz: policy=%s cpus=%d events=%d dropped=%d\n",
         policy_name(snapshot.policy), snapshot.cpus, snapshot.events,
         snapshot.dropped);
  print_axis(min_tick, max_tick);
  for(int cpu = 0; cpu < snapshot.cpus && cpu < NCPU; cpu++)
    printf("CPU%d | %s |\n", cpu, lanes[cpu]);
  printf("       1 cell ~= %d scheduler tick(s); '.' = idle / untraced\n",
         (int)((max_tick - min_tick + CHART_WIDTH - 1) / CHART_WIDTH));
}

/**
 * print_legend 输出 worker 标识、PID 和策略输入参数。
 *
 * @param workers worker 元数据。
 * @param count worker 数量。
 */
static void
print_legend(struct worker_info workers[], int count)
{
  printf("\nlegend:\n");
  for(int i = 0; i < count; i++)
    printf("  %c = worker%d pid=%d hint=%d weight=%d\n",
           workers[i].glyph, workers[i].id, workers[i].pid,
           workers[i].hint, workers[i].weight);
}

/**
 * print_events 输出关键 RUN_STOP 与 MLFQ boost 标记。
 */
static void
print_events(void)
{
  printf("\nevents:\n");
  for(int i = 0; i < snapshot.events; i++){
    struct schedtrace_event *event = &snapshot.events_buffer[i];
    if(event->event_type == SCHEDTRACE_EVENT_RUN_STOP){
      printf("  t=%d cpu=%d pid=%d reason=%s runtime=%d dispatches=%d level=%d weight=%d vruntime=%d\n",
             (int)event->scheduler_clock, event->cpu_id, event->pid,
             reason_name(event->stop_reason), (int)event->runtime_ticks,
             (int)event->dispatches, event->mlfq_level, event->weight,
             (int)event->vruntime);
    } else if(event->event_type == SCHEDTRACE_EVENT_MLFQ_BOOST){
      printf("  t=%d cpu=%d MLFQ_BOOST epoch=%d\n",
             (int)event->scheduler_clock, event->cpu_id, (int)event->mlfq_epoch);
    }
  }
}

/**
 * run_demo 根据当前编译策略选择默认教学场景并打印字符图。
 *
 * @return 成功返回 0；worker、trace 或读取失败返回 -1。
 */
static int
run_demo(struct demo_options *options)
{
  struct sched_stats stats;
  struct worker_info workers[MAX_WORKERS];
  int count = 3;
  int long_mode = options->seconds > 0;
  int total_ticks;
  struct worker_spec specs[MAX_WORKERS] = {
    {0, 8, 1024, 8, 0},
    {1, 8, 1024, 8, 0},
    {2, 8, 1024, 8, 0},
  };

  if(sched_get_stats(&stats) < 0)
    return -1;

  if(long_mode){
    count = options->workers;
    total_ticks = options->seconds * TICKS_PER_SECOND * XV6_CPUS;
    if(stats.policy == SCHED_POLICY_FIFO ||
       stats.policy == SCHED_POLICY_SJF){
      fill_scaled_burst_specs(specs, count, total_ticks);
    } else if(stats.policy == SCHED_POLICY_STCF){
      if(run_scaled_stcf_workers(count, options->seconds, workers) < 0)
        return -1;
      print_chart(workers, count);
      print_legend(workers, count);
      printf("  long demo: seconds=%d workers=%d\n", options->seconds, count);
      printf("  hint 是实验输入，不表示内核能预测未来 CPU burst。\n");
      print_events();
      printf("schedviz: OK policy=%s\n", policy_name(snapshot.policy));
      return 0;
    } else {
      fill_scaled_burst_specs(specs, count, total_ticks);
      if(stats.policy == SCHED_POLICY_CFS)
        apply_cfs_weight_pattern(specs, count);
    }
  } else if(stats.policy == SCHED_POLICY_FIFO ||
     stats.policy == SCHED_POLICY_SJF){
    specs[0].hint = specs[0].runtime_ticks = 8;
    specs[1].hint = specs[1].runtime_ticks = 2;
    specs[2].hint = specs[2].runtime_ticks = 5;
  } else if(stats.policy == SCHED_POLICY_STCF){
    count = 2;
    if(run_stcf_workers(workers) < 0)
      return -1;
    print_chart(workers, count);
    print_legend(workers, count);
    printf("  hint 是实验输入，不表示内核能预测未来 CPU burst。\n");
    print_events();
    printf("schedviz: OK policy=%s\n", policy_name(snapshot.policy));
    return 0;
  } else if(stats.policy == SCHED_POLICY_MLFQ){
    count = 1;
    specs[0].hint = 16;
    specs[0].runtime_ticks = 16;
  } else if(stats.policy == SCHED_POLICY_CFS){
    count = 2;
    specs[0].hint = 16;
    specs[0].weight = 2048;
    specs[0].wall_ticks = 30;
    specs[1].hint = 16;
    specs[1].weight = 512;
    specs[1].wall_ticks = 30;
  }

  if(run_standard_workers(specs, count, workers) < 0)
    return -1;
  print_chart(workers, count);
  print_legend(workers, count);
  if(long_mode)
    printf("  long demo: seconds=%d workers=%d\n", options->seconds, count);
  if(stats.policy == SCHED_POLICY_SJF)
    printf("  hint 是实验输入，不表示内核能预测未来 CPU burst。\n");
  if(stats.policy == SCHED_POLICY_CFS)
    printf("  Minimal CFS 图形展示当前教学实现的权重效果，不等同 Linux CFS。\n");
  print_events();
  printf("schedviz: OK policy=%s\n", policy_name(snapshot.policy));
  return 0;
}

/**
 * dump_trace 输出稳定、版本化、逐行可解析的 SCHEDTRACE 文本协议。
 *
 * @return 成功输出完整协议返回 0；读取失败返回 -1。
 */
static int
dump_trace(void)
{
  if(current_snapshot() < 0)
    return -1;
  printf("SCHEDTRACE version=%d policy=%s cpus=%d events=%d dropped=%d\n",
         snapshot.version, policy_name(snapshot.policy), snapshot.cpus,
         snapshot.events, snapshot.dropped);
  for(int i = 0; i < snapshot.events; i++){
    struct schedtrace_event *event = &snapshot.events_buffer[i];
    printf("SCHEDTRACE event seq=%d ts=%d tick=%d cpu=%d pid=%d type=%s reason=%s state=%d slice=%d runtime=%d dispatches=%d remaining=%d level=%d epoch=%d weight=%d vruntime=%d name=%s\n",
           (int)event->seq, (int)event->timestamp,
           (int)event->scheduler_clock, event->cpu_id, event->pid,
           event_type_name(event->event_type), reason_name(event->stop_reason),
           event->process_state, (int)event->slice_ticks,
           (int)event->runtime_ticks, (int)event->dispatches,
           (int)event->remaining_hint, event->mlfq_level,
           (int)event->mlfq_epoch, event->weight, (int)event->vruntime,
           event->process_name);
  }
  printf("SCHEDTRACE done status=0\n");
  return 0;
}

/**
 * main 解析 schedviz 子命令并执行 demo 或 dump。
 *
 * @param argc 参数数量。
 * @param argv 参数数组。
 * @return 成功返回 0；参数错误返回 2；运行失败返回 1。
 */
int
main(int argc, char **argv)
{
  struct demo_options options;

  if(argc < 2){
    usage();
    exit(2);
  }
  if(strcmp(argv[1], "demo") == 0){
    if(parse_demo_options(argc, argv, &options) < 0){
      usage();
      exit(2);
    }
    exit(run_demo(&options) == 0 ? 0 : 1);
  }
  if(strcmp(argv[1], "dump") == 0){
    if(argc != 2){
      usage();
      exit(2);
    }
    exit(dump_trace() == 0 ? 0 : 1);
  }
  usage();
  exit(2);
}
