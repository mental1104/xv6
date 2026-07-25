#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "kernel/schedstat.h"
#include "kernel/schedtrace_abi.h"
#include "user/user.h"
#include "user/paths.h"

#define CONDVAR_CHILDREN 2
#define CONDVAR_OBSERVE_RETRIES 200

static struct schedtrace_snapshot snapshot;
static int condvar_live_children[CONDVAR_CHILDREN];

/**
 * fail 打印失败原因并以非零状态退出。
 *
 * @param message 稳定失败说明。
 */
static void
fail(char *message)
{
  printf("schedtracetest: FAIL: %s\n", message);
  exit(1);
}

/** burn 执行一段 CPU 工作，用于稳定产生 timer tick。 */
static void
burn(void)
{
  volatile unsigned long value = 1;
  for(int i = 0; i < 100000; i++)
    value = value * 1664525 + 1013904223;
}

/**
 * consume_runtime_ticks 让当前进程实际运行指定数量的调度 tick。
 *
 * @param ticks 目标 runtime tick 数，必须为正。
 */
static void
consume_runtime_ticks(int ticks)
{
  struct sched_stats start;
  struct sched_stats current;

  if(sched_get_stats(&start) < 0)
    fail("sched_get_stats start");
  do {
    burn();
    if(sched_get_stats(&current) < 0)
      fail("sched_get_stats current");
  } while(current.runtime_ticks - start.runtime_ticks < (unsigned long)ticks);
}

/**
 * read_snapshot 读取当前 trace 快照。
 *
 * @param max_events 调用者提供的事件容量。
 */
static void
read_snapshot(int max_events)
{
  if(schedtrace(SCHEDTRACE_OP_READ, &snapshot, max_events) < 0)
    fail("read snapshot");
}

/**
 * release_worker 释放被 pipe gate 阻塞的 worker。
 *
 * @param fd worker 等待的 pipe 写端。
 */
static void
release_worker(int fd)
{
  if(write(fd, "x", 1) != 1)
    fail("release worker");
  close(fd);
}

/**
 * run_worker 创建一个可被 trace 过滤器观察的 CPU worker。
 *
 * @param ticks worker 消耗的 runtime tick 数。
 * @param release_fd 返回 parent 用于释放 worker 的 pipe 写端。
 * @return 子进程 PID；fork 失败时直接 fail。
 */
static int
run_worker(int ticks, int *release_fd)
{
  int gate[2];
  int ready[2];
  int pid;
  char token;

  if(pipe(ready) < 0)
    fail("pipe ready");
  if(pipe(gate) < 0)
    fail("pipe worker");
  pid = fork();
  if(pid < 0)
    fail("fork worker");
  if(pid == 0){
    close(ready[0]);
    close(gate[1]);
    if(sched_set_hint(ticks) < 0)
      exit(1);
    if(write(ready[1], "r", 1) != 1)
      exit(1);
    close(ready[1]);
    if(read(gate[0], &token, 1) != 1)
      exit(1);
    close(gate[0]);
    consume_runtime_ticks(ticks);
    exit(0);
  }
  close(ready[1]);
  close(gate[0]);
  if(read(ready[0], &token, 1) != 1)
    fail("wait worker ready");
  close(ready[0]);
  *release_fd = gate[1];
  return pid;
}

/**
 * trace_one_worker 执行一次完整 reset/watch/start/stop/read 流程。
 *
 * @param ticks worker 消耗的 runtime tick 数。
 * @param max_events read 时传入的容量。
 */
static void
trace_one_worker(int ticks, int max_events)
{
  int pid;
  int release_fd;

  if(schedtrace(SCHEDTRACE_OP_RESET, 0, 0) < 0)
    fail("reset");
  pid = run_worker(ticks, &release_fd);
  if(schedtrace(SCHEDTRACE_OP_WATCH_PID, 0, pid) < 0)
    fail("watch pid");
  if(schedtrace(SCHEDTRACE_OP_WATCH_PID, 0, pid) < 0)
    fail("watch duplicate pid");
  if(schedtrace(SCHEDTRACE_OP_START, 0, 0) < 0)
    fail("start");
  release_worker(release_fd);
  wait(0);
  if(schedtrace(SCHEDTRACE_OP_STOP, 0, 0) < 0)
    fail("stop");
  read_snapshot(max_events);
}

/** verify_default_off 确认默认关闭时 CPU worker 不会产生事件。 */
static void
verify_default_off(void)
{
  int pid;
  int release_fd;

  if(schedtrace(SCHEDTRACE_OP_RESET, 0, 0) < 0)
    fail("reset default off");
  pid = run_worker(2, &release_fd);
  if(schedtrace(SCHEDTRACE_OP_WATCH_PID, 0, pid) < 0)
    fail("watch default off");
  release_worker(release_fd);
  wait(0);
  read_snapshot(SCHEDTRACE_MAX_EVENTS);
  if(snapshot.events != 0)
    fail("default off recorded events");
}

/** verify_basic_events 校验 RUN_START/RUN_STOP 可配对且时间与 CPU 字段合法。 */
static void
verify_basic_events(void)
{
  int starts = 0;
  int stops = 0;
  unsigned long last_ts = 0;

  trace_one_worker(4, SCHEDTRACE_MAX_EVENTS);
  if(snapshot.version != SCHEDTRACE_VERSION || snapshot.events <= 0){
    printf("schedtracetest: header version=%d events=%d dropped=%d active=%d\n",
           snapshot.version, snapshot.events, snapshot.dropped, snapshot.active);
    fail("basic snapshot header");
  }
  for(int i = 0; i < snapshot.events; i++){
    struct schedtrace_event *event = &snapshot.events_buffer[i];
    if(event->timestamp < last_ts)
      fail("timestamp regressed");
    last_ts = event->timestamp;
    if(event->cpu_id < 0 || event->cpu_id >= NCPU)
      fail("invalid cpu id");
    if(event->event_type == SCHEDTRACE_EVENT_RUN_START)
      starts++;
    if(event->event_type == SCHEDTRACE_EVENT_RUN_STOP)
      stops++;
  }
  if(starts == 0 || starts != stops)
    fail("start stop pairing");
}

/** verify_capacity_shortage 确认调用者容量不足会通过 dropped 暴露。 */
static void
verify_capacity_shortage(void)
{
  trace_one_worker(4, 1);
  if(snapshot.events != 1 || snapshot.dropped == 0)
    fail("short read capacity did not report dropped");
}

/** verify_pid_filter 确认未注册 PID 不会污染当前 session。 */
static void
verify_pid_filter(void)
{
  int watched;
  int ignored;
  int watched_release_fd;
  int ignored_release_fd;
  int saw_watched = 0;

  if(schedtrace(SCHEDTRACE_OP_RESET, 0, 0) < 0)
    fail("reset filter");
  watched = run_worker(3, &watched_release_fd);
  ignored = run_worker(3, &ignored_release_fd);
  if(schedtrace(SCHEDTRACE_OP_WATCH_PID, 0, watched) < 0)
    fail("watch filter pid");
  if(schedtrace(SCHEDTRACE_OP_START, 0, 0) < 0)
    fail("start filter");
  release_worker(watched_release_fd);
  release_worker(ignored_release_fd);
  wait(0);
  wait(0);
  if(schedtrace(SCHEDTRACE_OP_STOP, 0, 0) < 0)
    fail("stop filter");
  read_snapshot(SCHEDTRACE_MAX_EVENTS);
  for(int i = 0; i < snapshot.events; i++){
    if(snapshot.events_buffer[i].pid == ignored)
      fail("ignored pid recorded");
    if(snapshot.events_buffer[i].pid == watched)
      saw_watched = 1;
  }
  if(!saw_watched)
    fail("watched pid missing");
}

/** verify_repeat_reset 确认第二次 session 不混入旧事件。 */
static void
verify_repeat_reset(void)
{
  int first_pid;
  int second_pid;

  trace_one_worker(2, SCHEDTRACE_MAX_EVENTS);
  first_pid = snapshot.events_buffer[0].pid;
  trace_one_worker(2, SCHEDTRACE_MAX_EVENTS);
  second_pid = snapshot.events_buffer[0].pid;
  if(first_pid == second_pid)
    fail("pid did not advance for repeat check");
  for(int i = 0; i < snapshot.events; i++)
    if(snapshot.events_buffer[i].pid == first_pid)
      fail("old session leaked into new trace");
}

/** verify_invalid_inputs 覆盖非法 operation、空容量和不存在 PID。 */
static void
verify_invalid_inputs(void)
{
  if(schedtrace(9999, 0, 0) >= 0)
    fail("invalid op accepted");
  if(schedtrace(SCHEDTRACE_OP_WATCH_PID, 0, 9999) >= 0)
    fail("invalid pid accepted");
  trace_one_worker(2, 0);
  if(snapshot.events != 0 || snapshot.dropped == 0)
    fail("empty buffer did not expose dropped");
}

/** verify_schedviz_args 通过 exec 黑盒验证 schedviz 参数错误返回非零。 */
static void
verify_schedviz_args(void)
{
  int status = 0;
  int pid = fork();
  char *argv[] = {XV6_USR_BIN_PATH("schedviz"), "bogus", 0};

  if(pid < 0)
    fail("fork schedviz");
  if(pid == 0){
    exec(XV6_USR_BIN_PATH("schedviz"), argv);
    exit(127);
  }
  wait(&status);
  if(status == 0)
    fail("schedviz invalid args returned zero");
}

/** 子进程通过结果管道返回的一次读取结果。 */
struct condvar_reader_result {
  int pid;
  int bytes;
  char value;
};

/**
 * condvar_terminate_children 终止并回收条件等待场景中仍可能存活的子进程。
 *
 * 该函数只用于失败清理；kill() 会让阻塞在 pipe read 的子进程离开睡眠，随后由
 * wait() 回收，避免失败用例污染后续回归。
 */
static void
condvar_terminate_children(void)
{
  for(int i = 0; i < CONDVAR_CHILDREN; i++)
    if(condvar_live_children[i] > 0)
      kill(condvar_live_children[i]);
  for(int i = 0; i < CONDVAR_CHILDREN; i++)
    if(condvar_live_children[i] > 0)
      wait(0);
}

/**
 * condvar_fail 清理条件等待场景并交给统一失败出口报告。
 *
 * @param message 描述未满足的谓词、睡眠、唤醒或重检契约。
 */
static void
condvar_fail(char *message)
{
  condvar_terminate_children();
  fail(message);
}

/**
 * condvar_read_exact 从管道读取固定字节数。
 *
 * @param fd 可读文件描述符。
 * @param buffer 接收数据的缓冲区。
 * @param size 必须读取的字节数。
 * @return 读满返回 0；遇到 EOF 或错误返回 -1。
 */
static int
condvar_read_exact(int fd, void *buffer, int size)
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
 * condvar_write_exact 向管道写入固定字节数。
 *
 * @param fd 可写文件描述符。
 * @param buffer 只读输入缓冲区，所有权仍归调用者。
 * @param size 必须写入的字节数。
 * @return 写满返回 0；管道关闭或写入失败返回 -1。
 */
static int
condvar_write_exact(int fd, const void *buffer, int size)
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
 * condvar_trace_start 为指定子进程建立新的调度轨迹会话。
 *
 * @param pids 需要观察的子进程 PID 数组。
 * @param count PID 数量，不能超过 schedtrace 过滤器容量。
 */
static void
condvar_trace_start(int pids[], int count)
{
  if(schedtrace(SCHEDTRACE_OP_RESET, 0, 0) < 0)
    condvar_fail("condvar trace reset");
  for(int i = 0; i < count; i++)
    if(schedtrace(SCHEDTRACE_OP_WATCH_PID, 0, pids[i]) < 0)
      condvar_fail("condvar trace watch pid");
  if(schedtrace(SCHEDTRACE_OP_START, 0, 0) < 0)
    condvar_fail("condvar trace start");
}

/** 停止条件等待场景的调度轨迹会话。 */
static void
condvar_trace_stop(void)
{
  if(schedtrace(SCHEDTRACE_OP_STOP, 0, 0) < 0)
    condvar_fail("condvar trace stop");
}

/** 读取完整调度轨迹，并拒绝任何可能破坏判定的事件丢失。 */
static void
condvar_trace_read(void)
{
  read_snapshot(SCHEDTRACE_MAX_EVENTS);
  if(snapshot.dropped != 0)
    condvar_fail("condvar trace events dropped");
}

/**
 * condvar_count_events 统计某进程满足类型与停止原因的调度事件。
 *
 * @param pid 目标子进程 PID。
 * @param event_type SCHEDTRACE_EVENT_*；传 0 表示不限制事件类型。
 * @param stop_reason SCHEDTRACE_REASON_*；传负数表示不限制停止原因。
 * @param after_seq 只统计序号严格大于该值的事件。
 * @return 匹配事件数量。
 */
static int
condvar_count_events(int pid, int event_type, int stop_reason,
                     unsigned long after_seq)
{
  int count = 0;

  for(int i = 0; i < snapshot.events; i++){
    struct schedtrace_event *event = &snapshot.events_buffer[i];
    if(event->pid != pid || event->seq <= after_seq)
      continue;
    if(event_type != 0 && event->event_type != event_type)
      continue;
    if(stop_reason >= 0 && event->stop_reason != stop_reason)
      continue;
    count++;
  }
  return count;
}

/** @return 当前快照中的最大事件序号；空快照返回 0。 */
static unsigned long
condvar_last_sequence(void)
{
  unsigned long result = 0;

  for(int i = 0; i < snapshot.events; i++)
    if(snapshot.events_buffer[i].seq > result)
      result = snapshot.events_buffer[i].seq;
  return result;
}

/**
 * condvar_spawn_reader 创建一个由 gate 控制、随后读取共享 data pipe 的子进程。
 *
 * @param data_read 子进程读取谓词状态的管道读端。
 * @param data_write data pipe 写端，子进程会关闭。
 * @param ready_read ready pipe 读端，子进程会关闭。
 * @param ready_write 子进程发布“即将等待 gate”的通知端。
 * @param gate_read 父进程释放阶段屏障的管道读端。
 * @param gate_write gate pipe 写端，子进程会关闭。
 * @param result_read result pipe 读端，子进程会关闭。
 * @param result_write 子进程返回读取结果的管道写端。
 * @return 子进程 PID；fork 失败时返回 -1。
 */
static int
condvar_spawn_reader(int data_read, int data_write,
                     int ready_read, int ready_write,
                     int gate_read, int gate_write,
                     int result_read, int result_write)
{
  int pid = fork();

  if(pid != 0)
    return pid;

  close(data_write);
  close(ready_read);
  close(gate_write);
  close(result_read);

  char token = 'r';
  if(condvar_write_exact(ready_write, &token, 1) < 0)
    exit(1);
  close(ready_write);
  if(condvar_read_exact(gate_read, &token, 1) < 0)
    exit(1);
  close(gate_read);

  struct condvar_reader_result result;
  result.pid = getpid();
  result.bytes = read(data_read, &result.value, 1);
  close(data_read);
  if(condvar_write_exact(result_write, &result, sizeof(result)) < 0)
    exit(1);
  close(result_write);
  exit(result.bytes == 1 ? 0 : 1);
}

/**
 * condvar_wait_for_initial_sleeps 等待两个 reader 都因空 data pipe 再次睡眠。
 *
 * gate 唤醒先产生 RUN_START；随后 piperead() 检查空谓词并调用 sleep()，调度器
 * 记录 RUN_STOP/SLEEP。轮询依据可观察状态事件，而不是依赖人工抢时机。
 */
static void
condvar_wait_for_initial_sleeps(int pids[])
{
  for(int attempt = 0; attempt < CONDVAR_OBSERVE_RETRIES; attempt++){
    condvar_trace_read();
    if(condvar_count_events(pids[0], SCHEDTRACE_EVENT_RUN_STOP,
                            SCHEDTRACE_REASON_SLEEP, 0) > 0 &&
       condvar_count_events(pids[1], SCHEDTRACE_EVENT_RUN_STOP,
                            SCHEDTRACE_REASON_SLEEP, 0) > 0)
      return;
    sleep(1);
  }
  condvar_fail("readers did not reach pipe sleep");
}

/**
 * condvar_wait_for_ineffective_wakeup 等待一次写入形成“两者运行、一退一睡”。
 *
 * @param pids 两个 reader PID。
 * @return 被无效唤醒后重新睡眠的 reader 下标；超时直接失败。
 */
static int
condvar_wait_for_ineffective_wakeup(int pids[])
{
  for(int attempt = 0; attempt < CONDVAR_OBSERVE_RETRIES; attempt++){
    condvar_trace_read();
    if(condvar_count_events(pids[0], SCHEDTRACE_EVENT_RUN_START, -1, 0) > 0 &&
       condvar_count_events(pids[1], SCHEDTRACE_EVENT_RUN_START, -1, 0) > 0){
      int slept0 = condvar_count_events(pids[0], SCHEDTRACE_EVENT_RUN_STOP,
                                        SCHEDTRACE_REASON_SLEEP, 0) > 0;
      int slept1 = condvar_count_events(pids[1], SCHEDTRACE_EVENT_RUN_STOP,
                                        SCHEDTRACE_REASON_SLEEP, 0) > 0;
      int exited0 = condvar_count_events(pids[0], SCHEDTRACE_EVENT_RUN_STOP,
                                         SCHEDTRACE_REASON_EXIT, 0) > 0;
      int exited1 = condvar_count_events(pids[1], SCHEDTRACE_EVENT_RUN_STOP,
                                         SCHEDTRACE_REASON_EXIT, 0) > 0;
      if(slept0 && exited1 && !exited0 && !slept1)
        return 0;
      if(slept1 && exited0 && !exited1 && !slept0)
        return 1;
    }
    sleep(1);
  }
  condvar_fail("wakeup did not produce one exit and one re-sleep");
  return -1;
}

/**
 * verify_preexisting_pipe_data 验证谓词已成立时 reader 不依赖历史 wakeup。
 *
 * 父进程先写入字节，再释放 reader。调度轨迹应只包含 gate 唤醒后的运行与退出，
 * 不应出现 data pipe 上的 RUN_STOP/SLEEP。
 */
static void
verify_preexisting_pipe_data(void)
{
  int data[2];
  int ready[2];
  int gate[2];
  int result_pipe[2];
  int status = 0;
  int pid;
  char token;
  struct condvar_reader_result result;

  memset(condvar_live_children, 0, sizeof(condvar_live_children));
  if(pipe(data) < 0 || pipe(ready) < 0 || pipe(gate) < 0 || pipe(result_pipe) < 0)
    condvar_fail("preexisting pipe setup");

  pid = condvar_spawn_reader(data[0], data[1], ready[0], ready[1],
                             gate[0], gate[1], result_pipe[0], result_pipe[1]);
  if(pid < 0)
    condvar_fail("preexisting fork");
  condvar_live_children[0] = pid;

  close(data[0]);
  close(ready[1]);
  close(gate[0]);
  close(result_pipe[1]);
  if(condvar_read_exact(ready[0], &token, 1) < 0)
    condvar_fail("preexisting ready");
  close(ready[0]);

  token = 'P';
  if(condvar_write_exact(data[1], &token, 1) < 0)
    condvar_fail("preexisting data write");
  condvar_trace_start(&pid, 1);
  token = 'g';
  if(condvar_write_exact(gate[1], &token, 1) < 0)
    condvar_fail("preexisting gate release");
  close(gate[1]);
  close(data[1]);

  if(wait(&status) != pid || status != 0)
    condvar_fail("preexisting child status");
  condvar_live_children[0] = 0;
  condvar_trace_stop();
  condvar_trace_read();
  if(condvar_read_exact(result_pipe[0], &result, sizeof(result)) < 0)
    condvar_fail("preexisting result");
  close(result_pipe[0]);
  if(result.pid != pid || result.bytes != 1 || result.value != 'P')
    condvar_fail("preexisting predicate result");
  if(condvar_count_events(pid, SCHEDTRACE_EVENT_RUN_STOP,
                          SCHEDTRACE_REASON_SLEEP, 0) != 0)
    condvar_fail("preexisting data unexpectedly slept");
  if(condvar_count_events(pid, SCHEDTRACE_EVENT_RUN_STOP,
                          SCHEDTRACE_REASON_EXIT, 0) == 0)
    condvar_fail("preexisting exit event missing");
}

/**
 * verify_pipe_wakeup_rechecks_predicate 验证 pipe 的完整条件等待协议。
 *
 * 两个 reader 先稳定睡在同一 data channel。第一次只写一个字节时，wakeup() 会让
 * 两者都重新运行；一个 reader 消费字节并退出，另一个重新持有 pipe lock 后发现
 * 谓词仍为假，再次进入 RUN_STOP/SLEEP。第二次写入才让剩余 reader 完成。
 */
static void
verify_pipe_wakeup_rechecks_predicate(void)
{
  int data[2];
  int ready[2];
  int gate[2];
  int result_pipe[2];
  int pids[CONDVAR_CHILDREN];
  int status;
  char ready_tokens[CONDVAR_CHILDREN];
  char gate_tokens[CONDVAR_CHILDREN] = {'g', 'g'};
  struct condvar_reader_result results[CONDVAR_CHILDREN];

  memset(condvar_live_children, 0, sizeof(condvar_live_children));
  if(pipe(data) < 0 || pipe(ready) < 0 || pipe(gate) < 0 || pipe(result_pipe) < 0)
    condvar_fail("recheck pipe setup");

  for(int i = 0; i < CONDVAR_CHILDREN; i++){
    pids[i] = condvar_spawn_reader(data[0], data[1], ready[0], ready[1],
                                   gate[0], gate[1], result_pipe[0], result_pipe[1]);
    if(pids[i] < 0)
      condvar_fail("recheck fork");
    condvar_live_children[i] = pids[i];
  }

  close(data[0]);
  close(ready[1]);
  close(gate[0]);
  close(result_pipe[1]);
  if(condvar_read_exact(ready[0], ready_tokens, sizeof(ready_tokens)) < 0)
    condvar_fail("recheck readers ready");
  close(ready[0]);

  condvar_trace_start(pids, CONDVAR_CHILDREN);
  if(condvar_write_exact(gate[1], gate_tokens, sizeof(gate_tokens)) < 0)
    condvar_fail("recheck gate release");
  close(gate[1]);
  condvar_wait_for_initial_sleeps(pids);
  condvar_trace_stop();

  // 清空 gate 阶段轨迹，只观察 data pipe 的一次状态改变和重检结果。
  condvar_trace_start(pids, CONDVAR_CHILDREN);
  char first = 'A';
  if(condvar_write_exact(data[1], &first, 1) < 0)
    condvar_fail("recheck first write");
  int loser = condvar_wait_for_ineffective_wakeup(pids);
  unsigned long first_phase_end = condvar_last_sequence();

  char second = 'B';
  if(condvar_write_exact(data[1], &second, 1) < 0)
    condvar_fail("recheck second write");
  close(data[1]);

  for(int i = 0; i < CONDVAR_CHILDREN; i++){
    int waited = wait(&status);
    if(waited < 0 || status != 0)
      condvar_fail("recheck child status");
    if(waited == pids[0])
      condvar_live_children[0] = 0;
    else if(waited == pids[1])
      condvar_live_children[1] = 0;
    else
      condvar_fail("recheck unexpected child");
  }
  condvar_trace_stop();
  condvar_trace_read();

  if(condvar_count_events(pids[loser], SCHEDTRACE_EVENT_RUN_START,
                          -1, first_phase_end) == 0)
    condvar_fail("re-slept reader was not started by second write");
  if(condvar_count_events(pids[loser], SCHEDTRACE_EVENT_RUN_STOP,
                          SCHEDTRACE_REASON_EXIT, first_phase_end) == 0)
    condvar_fail("re-slept reader did not exit after second write");

  if(condvar_read_exact(result_pipe[0], results, sizeof(results)) < 0)
    condvar_fail("recheck result read");
  close(result_pipe[0]);

  int saw_a = 0;
  int saw_b = 0;
  int saw_pid[CONDVAR_CHILDREN] = {0, 0};
  for(int i = 0; i < CONDVAR_CHILDREN; i++){
    if(results[i].bytes != 1)
      condvar_fail("reader continued after ineffective wakeup");
    if(results[i].value == 'A')
      saw_a++;
    else if(results[i].value == 'B')
      saw_b++;
    else
      condvar_fail("reader returned unexpected byte");
    if(results[i].pid == pids[0])
      saw_pid[0]++;
    else if(results[i].pid == pids[1])
      saw_pid[1]++;
    else
      condvar_fail("result pid not owned by scenario");
  }
  if(saw_a != 1 || saw_b != 1 || saw_pid[0] != 1 || saw_pid[1] != 1)
    condvar_fail("reader results were not one-to-one");
}

/** 执行条件变量概念对应的 xv6 pipe/sleep/wakeup 验证闭环。 */
static void
verify_condition_wait_protocol(void)
{
  verify_preexisting_pipe_data();
  verify_pipe_wakeup_rechecks_predicate();
}

/**
 * main 执行 schedtrace、schedviz 或聚焦条件等待的 guest-first 回归。
 *
 * @param argc 参数数量；无参数运行原 schedtrace 回归，`condvar` 只运行条件等待闭环。
 * @param argv 参数数组。
 * @return 成功 exit(0)，任一断言失败 exit(1)。
 */
int
main(int argc, char **argv)
{
  if(argc == 2 && strcmp(argv[1], "condvar") == 0){
    verify_condition_wait_protocol();
    printf("condvartest: OK\n");
    exit(0);
  }
  if(argc != 1)
    fail("usage: schedtracetest [condvar]");

  verify_default_off();
  verify_basic_events();
  verify_capacity_shortage();
  verify_pid_filter();
  verify_repeat_reset();
  verify_invalid_inputs();
  verify_schedviz_args();
  printf("schedtracetest: OK\n");
  exit(0);
}
