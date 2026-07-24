#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "kernel/schedstat.h"
#include "kernel/schedtrace_abi.h"
#include "user/user.h"
#include "user/paths.h"

static struct schedtrace_snapshot snapshot;

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

/**
 * main 执行 schedtrace 和 schedviz 的 guest-first 回归。
 *
 * @return 成功 exit(0)，任一断言失败 exit(1)。
 */
int
main(int argc, char **argv)
{
  (void)argc;
  (void)argv;

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
