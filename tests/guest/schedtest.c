#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/schedstat.h"
#include "kernel/schedtrace_abi.h"
#include "user/user.h"

#define WORKERS 3
#define MLFQ_CPU_TICKS 8
#define MLFQ_COMPETITION_CPU_TICKS 80
#define MLFQ_INTERACTIVE_ROUNDS 4
#define MLFQ_INTERACTIVE_SLEEP_TICKS 2
#define MLFQ_BOOST_RUNTIME_LIMIT 96

struct worker_result {
  int id;
  int completed_tick;
  int max_mlfq_level;
  unsigned long initial_mlfq_epoch;
  struct sched_stats stats;
};

static struct schedtrace_snapshot mlfq_snapshot;

static void
burn(void)
{
  volatile uint64 value = 1;
  for(int i = 0; i < 100000; i++)
    value = value * 1664525 + 1013904223;
}

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
  } while(current.runtime_ticks - start.runtime_ticks < (uint64)ticks);
  return 0;
}

/**
 * write_result_details 写回 worker 的完成顺序、MLFQ 历史峰值和最终状态。
 *
 * @param fd 父进程读取结果的管道写端。
 * @param id 场景内稳定 worker 编号。
 * @param initial_epoch worker 开始时看到的 boost 代际；无关场景传 0。
 * @param max_level worker 运行期间观察到的最高 MLFQ 层级。
 */
static void
write_result_details(int fd, int id, unsigned long initial_epoch, int max_level)
{
  struct worker_result result;
  result.id = id;
  result.completed_tick = uptime();
  result.max_mlfq_level = max_level;
  result.initial_mlfq_epoch = initial_epoch;
  if(sched_get_stats(&result.stats) < 0)
    exit(1);
  if(write_exact(fd, &result, sizeof(result)) < 0)
    exit(1);
}

static void
write_result(int fd, int id)
{
  struct sched_stats stats;
  if(sched_get_stats(&stats) < 0)
    exit(1);
  write_result_details(fd, id, 0, stats.mlfq_level);
}

static void
barrier_worker(int id, int hint, int weight, int readyfd, int startfd,
               int resultfd)
{
  char token = 'r';

  if(sched_set_hint(hint) < 0 || sched_set_weight(weight) < 0)
    exit(1);
  if(write_exact(readyfd, &token, 1) < 0)
    exit(1);
  if(read_exact(startfd, &token, 1) < 0)
    exit(1);
  if(consume_runtime_ticks(hint) < 0)
    exit(1);
  write_result(resultfd, id);
  exit(0);
}

static int
run_barrier_workers(const int hints[WORKERS], const int weights[WORKERS],
                    struct worker_result results[WORKERS])
{
  int ready[2];
  int start[2];
  int result[2];
  char token;

  if(pipe(ready) < 0 || pipe(start) < 0 || pipe(result) < 0)
    return -1;

  for(int i = 0; i < WORKERS; i++){
    int pid = fork();
    if(pid < 0)
      return -1;
    if(pid == 0){
      close(ready[0]);
      close(start[1]);
      close(result[0]);
      barrier_worker(i, hints[i], weights[i], ready[1], start[0], result[1]);
    }
  }

  close(ready[1]);
  close(start[0]);
  close(result[1]);
  for(int i = 0; i < WORKERS; i++)
    if(read_exact(ready[0], &token, 1) < 0)
      return -1;
  for(int i = 0; i < WORKERS; i++)
    if(write_exact(start[1], "s", 1) < 0)
      return -1;
  for(int i = 0; i < WORKERS; i++)
    if(read_exact(result[0], &results[i], sizeof(results[i])) < 0)
      return -1;

  close(ready[0]);
  close(start[1]);
  close(result[0]);
  for(int i = 0; i < WORKERS; i++)
    wait(0);
  return 0;
}

static int
verify_fifo_or_sjf(int policy)
{
  const int hints[WORKERS] = {8, 2, 5};
  const int weights[WORKERS] = {1024, 1024, 1024};
  const int fifo_order[WORKERS] = {0, 1, 2};
  const int sjf_order[WORKERS] = {1, 2, 0};
  const int *expected = policy == SCHED_POLICY_FIFO ? fifo_order : sjf_order;
  struct worker_result results[WORKERS];

  if(run_barrier_workers(hints, weights, results) < 0)
    return -1;
  for(int i = 0; i < WORKERS; i++){
    if(results[i].id != expected[i]){
      printf("schedtest: order mismatch index=%d got=%d expected=%d\n",
             i, results[i].id, expected[i]);
      return -1;
    }
  }
  return 0;
}

static int
verify_rr(void)
{
  const int hints[WORKERS] = {8, 8, 8};
  const int weights[WORKERS] = {1024, 1024, 1024};
  struct worker_result results[WORKERS];

  if(run_barrier_workers(hints, weights, results) < 0)
    return -1;
  for(int i = 0; i < WORKERS; i++){
    if(results[i].stats.dispatches < 3){
      printf("schedtest: rr worker=%d dispatches=%d\n",
             results[i].id, (int)results[i].stats.dispatches);
      return -1;
    }
  }
  return 0;
}

static void
stcf_worker(int id, int hint, int readyfd, int startfd, int resultfd)
{
  char token = 'r';
  if(sched_set_hint(hint) < 0)
    exit(1);
  if(write_exact(readyfd, &token, 1) < 0)
    exit(1);
  if(read_exact(startfd, &token, 1) < 0)
    exit(1);
  if(consume_runtime_ticks(hint) < 0)
    exit(1);
  write_result(resultfd, id);
  exit(0);
}

static int
verify_stcf(void)
{
  int ready[2];
  int long_start[2];
  int short_start[2];
  int result[2];
  struct worker_result first;
  struct worker_result second;
  char token;

  if(pipe(ready) < 0 || pipe(long_start) < 0 ||
     pipe(short_start) < 0 || pipe(result) < 0)
    return -1;

  int long_pid = fork();
  if(long_pid < 0)
    return -1;
  if(long_pid == 0)
    stcf_worker(0, 12, ready[1], long_start[0], result[1]);

  int short_pid = fork();
  if(short_pid < 0)
    return -1;
  if(short_pid == 0)
    stcf_worker(1, 2, ready[1], short_start[0], result[1]);

  if(read_exact(ready[0], &token, 1) < 0 ||
     read_exact(ready[0], &token, 1) < 0)
    return -1;

  // Keep the controller shorter than the long worker so it can wake after
  // two ticks, release the delayed short worker, and expose STCF preemption.
  if(sched_set_hint(1) < 0)
    return -1;
  if(write_exact(long_start[1], "l", 1) < 0)
    return -1;
  sleep(2);
  if(write_exact(short_start[1], "s", 1) < 0)
    return -1;

  if(read_exact(result[0], &first, sizeof(first)) < 0 ||
     read_exact(result[0], &second, sizeof(second)) < 0)
    return -1;
  wait(0);
  wait(0);

  if(first.id != 1){
    printf("schedtest: stcf first=%d expected=1\n", first.id);
    return -1;
  }
  if(first.completed_tick >= second.completed_tick){
    printf("schedtest: stcf completion ticks short=%d long=%d\n",
           first.completed_tick, second.completed_tick);
    return -1;
  }
  return 0;
}

/** CPU 密集 worker 连续消耗时间片，并记录实际到达过的最低优先级。 */
static void
mlfq_cpu_worker(int resultfd)
{
  struct sched_stats initial;
  struct sched_stats current;
  int max_level = 0;

  if(sched_get_stats(&initial) < 0)
    exit(1);
  do {
    burn();
    if(sched_get_stats(&current) < 0)
      exit(1);
    if(current.mlfq_level > max_level)
      max_level = current.mlfq_level;
  } while(current.runtime_ticks - initial.runtime_ticks < MLFQ_CPU_TICKS);
  write_result_details(resultfd, 0, initial.mlfq_epoch, max_level);
  exit(0);
}

/**
 * mlfq_gaming_worker 在每个短 CPU 片段后主动睡眠，验证队列预算不会随 dispatch 清零。
 */
static void
mlfq_gaming_worker(int resultfd)
{
  struct sched_stats before;
  struct sched_stats after;
  int max_level = 0;

  if(sched_get_stats(&before) < 0)
    exit(1);
  for(int i = 0; i < 3; i++){
    do {
      burn();
      if(sched_get_stats(&after) < 0)
        exit(1);
      if(after.mlfq_level > max_level)
        max_level = after.mlfq_level;
    } while(after.runtime_ticks - before.runtime_ticks < (uint64)(i + 1));
    sleep(1);
  }
  write_result_details(resultfd, 1, before.mlfq_epoch, max_level);
  exit(0);
}

/** 长时 CPU worker 等待全局 priority boost，并回报 boost 前后的代际。 */
static void
mlfq_boost_worker(int resultfd)
{
  struct sched_stats initial;
  struct sched_stats current;
  int max_level = 0;

  if(sched_get_stats(&initial) < 0)
    exit(1);
  do {
    burn();
    if(sched_get_stats(&current) < 0)
      exit(1);
    if(current.mlfq_level > max_level)
      max_level = current.mlfq_level;
    if(current.runtime_ticks - initial.runtime_ticks > MLFQ_BOOST_RUNTIME_LIMIT)
      exit(1);
  } while(current.mlfq_epoch == initial.mlfq_epoch);
  write_result_details(resultfd, 2, initial.mlfq_epoch, max_level);
  exit(0);
}

static int
run_one_result_worker(void (*worker)(int), struct worker_result *result)
{
  int pipefd[2];
  if(pipe(pipefd) < 0)
    return -1;
  int pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(pipefd[0]);
    worker(pipefd[1]);
  }
  close(pipefd[1]);
  if(read_exact(pipefd[0], result, sizeof(*result)) < 0)
    return -1;
  close(pipefd[0]);
  wait(0);
  return 0;
}

/** 同步启动 CPU 密集任务，直到收到 start token 后持续占用 CPU。 */
static void
mlfq_competition_cpu_worker(int readyfd, int startfd, int resultfd)
{
  char token = 'r';
  if(write_exact(readyfd, &token, 1) < 0 ||
     read_exact(startfd, &token, 1) < 0)
    exit(1);
  if(consume_runtime_ticks(MLFQ_COMPETITION_CPU_TICKS) < 0)
    exit(1);
  write_result(resultfd, 0);
  exit(0);
}

/**
 * mlfq_interactive_worker 周期性睡眠后只执行极短控制路径，模拟交互任务的唤醒。
 */
static void
mlfq_interactive_worker(int readyfd, int startfd, int resultfd)
{
  struct sched_stats current;
  char token = 'r';
  int max_level = 0;

  if(write_exact(readyfd, &token, 1) < 0 ||
     read_exact(startfd, &token, 1) < 0)
    exit(1);
  for(int i = 0; i < MLFQ_INTERACTIVE_ROUNDS; i++){
    sleep(MLFQ_INTERACTIVE_SLEEP_TICKS);
    if(sched_get_stats(&current) < 0)
      exit(1);
    if(current.mlfq_level > max_level)
      max_level = current.mlfq_level;
  }
  write_result_details(resultfd, 1, 0, max_level);
  exit(0);
}

/**
 * verify_mlfq_trace 从混合负载快照中校验降级、睡眠唤醒、boost 和单核抢占。
 *
 * @param cpu_pid CPU 密集 worker PID。
 * @param interactive_pid 周期睡眠 worker PID。
 * @return 所有事件契约满足返回 0，否则输出缺失项并返回 -1。
 */
static int
verify_mlfq_trace(int cpu_pid, int interactive_pid)
{
  int saw_level1 = 0;
  int saw_level2 = 0;
  int saw_interactive_sleep = 0;
  int saw_interactive_resume = 0;
  int saw_boost = 0;
  int saw_higher_queue = 0;

  if(schedtrace(SCHEDTRACE_OP_READ, &mlfq_snapshot,
                SCHEDTRACE_MAX_EVENTS) < 0)
    return -1;
  if(mlfq_snapshot.policy != SCHED_POLICY_MLFQ ||
     mlfq_snapshot.events <= 0 || mlfq_snapshot.dropped != 0){
    printf("schedtest: mlfq trace policy=%d events=%d dropped=%d\n",
           mlfq_snapshot.policy, mlfq_snapshot.events, mlfq_snapshot.dropped);
    return -1;
  }

  for(int i = 0; i < mlfq_snapshot.events; i++){
    struct schedtrace_event *event = &mlfq_snapshot.events_buffer[i];
    if(event->event_type == SCHEDTRACE_EVENT_MLFQ_BOOST)
      saw_boost = 1;
    if(event->pid == cpu_pid &&
       event->event_type == SCHEDTRACE_EVENT_RUN_STOP){
      if(event->stop_reason == SCHEDTRACE_REASON_MLFQ_QUANTUM &&
         event->mlfq_level == 1)
        saw_level1 = 1;
      if(event->stop_reason == SCHEDTRACE_REASON_MLFQ_QUANTUM &&
         event->mlfq_level == 2)
        saw_level2 = 1;
      if(event->stop_reason == SCHEDTRACE_REASON_MLFQ_HIGHER_QUEUE)
        saw_higher_queue = 1;
    }
    if(event->pid == interactive_pid &&
       event->event_type == SCHEDTRACE_EVENT_RUN_STOP &&
       event->stop_reason == SCHEDTRACE_REASON_SLEEP &&
       event->mlfq_level == 0)
      saw_interactive_sleep = 1;
    if(saw_interactive_sleep && event->pid == interactive_pid &&
       event->event_type == SCHEDTRACE_EVENT_RUN_START &&
       event->mlfq_level == 0)
      saw_interactive_resume = 1;
  }

  if(!saw_level1 || !saw_level2 || !saw_interactive_sleep ||
     !saw_interactive_resume || !saw_boost){
    printf("schedtest: mlfq trace q1=%d q2=%d sleep=%d resume=%d boost=%d\n",
           saw_level1, saw_level2, saw_interactive_sleep,
           saw_interactive_resume, saw_boost);
    return -1;
  }
  (void)saw_higher_queue;
#if defined(XV6_CPUS) && XV6_CPUS == 1
  if(!saw_higher_queue){
    printf("schedtest: mlfq trace missing higher-queue preemption\n");
    return -1;
  }
#endif
  return 0;
}

/**
 * run_mlfq_competition 验证睡眠型任务优先完成，并保留可由 schedviz dump 读取的快照。
 *
 * @return 响应顺序、层级和 trace 契约均符合预期返回 0，否则返回 -1。
 */
static int
run_mlfq_competition(void)
{
  int ready[2];
  int start[2];
  int result[2];
  struct worker_result first;
  struct worker_result second;
  char token;

  if(pipe(ready) < 0 || pipe(start) < 0 || pipe(result) < 0)
    return -1;

  int cpu_pid = fork();
  if(cpu_pid < 0)
    return -1;
  if(cpu_pid == 0){
    close(ready[0]);
    close(start[1]);
    close(result[0]);
    mlfq_competition_cpu_worker(ready[1], start[0], result[1]);
  }

  int interactive_pid = fork();
  if(interactive_pid < 0)
    return -1;
  if(interactive_pid == 0){
    close(ready[0]);
    close(start[1]);
    close(result[0]);
    mlfq_interactive_worker(ready[1], start[0], result[1]);
  }

  close(ready[1]);
  close(start[0]);
  close(result[1]);
  if(read_exact(ready[0], &token, 1) < 0 ||
     read_exact(ready[0], &token, 1) < 0)
    return -1;

  if(schedtrace(SCHEDTRACE_OP_RESET, 0, 0) < 0 ||
     schedtrace(SCHEDTRACE_OP_WATCH_PID, 0, cpu_pid) < 0 ||
     schedtrace(SCHEDTRACE_OP_WATCH_PID, 0, interactive_pid) < 0 ||
     schedtrace(SCHEDTRACE_OP_START, 0, 0) < 0)
    return -1;
  if(write_exact(start[1], "s", 1) < 0 ||
     write_exact(start[1], "s", 1) < 0)
    return -1;
  close(start[1]);
  close(ready[0]);

  if(read_exact(result[0], &first, sizeof(first)) < 0 ||
     read_exact(result[0], &second, sizeof(second)) < 0)
    return -1;
  close(result[0]);
  wait(0);
  wait(0);
  if(schedtrace(SCHEDTRACE_OP_STOP, 0, 0) < 0)
    return -1;

  if(first.id != 1 || second.id != 0){
    printf("schedtest: mlfq response first=%d second=%d\n", first.id, second.id);
    return -1;
  }
  if(first.max_mlfq_level != 0 || first.stats.mlfq_level != 0){
    printf("schedtest: mlfq interactive max=%d final=%d\n",
           first.max_mlfq_level, first.stats.mlfq_level);
    return -1;
  }
  return verify_mlfq_trace(cpu_pid, interactive_pid);
}

/**
 * verify_mlfq 组合验证降级、反 gaming、交互响应和周期 boost 四个行为契约。
 */
static int
verify_mlfq(void)
{
  struct worker_result cpu;
  struct worker_result gamer;
  struct worker_result boost;

  if(run_one_result_worker(mlfq_cpu_worker, &cpu) < 0 ||
     run_one_result_worker(mlfq_gaming_worker, &gamer) < 0 ||
     run_mlfq_competition() < 0 ||
     run_one_result_worker(mlfq_boost_worker, &boost) < 0)
    return -1;

  if(cpu.max_mlfq_level != 2 || cpu.stats.dispatches < 3){
    printf("schedtest: mlfq cpu max_level=%d dispatch=%d\n",
           cpu.max_mlfq_level, (int)cpu.stats.dispatches);
    return -1;
  }
  if(gamer.max_mlfq_level == 0){
    printf("schedtest: mlfq gaming reset budget\n");
    return -1;
  }
  if(boost.stats.mlfq_epoch <= boost.initial_mlfq_epoch){
    printf("schedtest: mlfq boost initial=%d final=%d\n",
           (int)boost.initial_mlfq_epoch, (int)boost.stats.mlfq_epoch);
    return -1;
  }
  return 0;
}

static void
cfs_worker(int id, int weight, int readyfd, int startfd, int resultfd)
{
  int deadline;
  char token = 'r';

  if(sched_set_weight(weight) < 0)
    exit(1);
  if(write_exact(readyfd, &token, 1) < 0)
    exit(1);
  if(read_exact(startfd, &deadline, sizeof(deadline)) < 0)
    exit(1);
  while(uptime() < deadline)
    burn();
  write_result(resultfd, id);
  exit(0);
}

static int
verify_cfs(void)
{
  int ready[2];
  int start[2];
  int result[2];
  struct worker_result results[2];
  struct worker_result *heavy = 0;
  struct worker_result *light = 0;
  char token;
  int deadline;

  if(pipe(ready) < 0 || pipe(start) < 0 || pipe(result) < 0)
    return -1;

  for(int i = 0; i < 2; i++){
    int pid = fork();
    if(pid < 0)
      return -1;
    if(pid == 0)
      cfs_worker(i, i == 0 ? 2048 : 512, ready[1], start[0], result[1]);
  }

  if(read_exact(ready[0], &token, 1) < 0 ||
     read_exact(ready[0], &token, 1) < 0)
    return -1;
  deadline = uptime() + 30;
  if(write_exact(start[1], &deadline, sizeof(deadline)) < 0 ||
     write_exact(start[1], &deadline, sizeof(deadline)) < 0)
    return -1;
  if(read_exact(result[0], &results[0], sizeof(results[0])) < 0 ||
     read_exact(result[0], &results[1], sizeof(results[1])) < 0)
    return -1;
  wait(0);
  wait(0);

  for(int i = 0; i < 2; i++){
    if(results[i].id == 0)
      heavy = &results[i];
    else if(results[i].id == 1)
      light = &results[i];
  }
  if(heavy == 0 || light == 0)
    return -1;
  if(heavy->stats.runtime_ticks < light->stats.runtime_ticks * 2){
    printf("schedtest: cfs runtime heavy=%d light=%d\n",
           (int)heavy->stats.runtime_ticks, (int)light->stats.runtime_ticks);
    return -1;
  }
  return 0;
}

/** 多核 smoke 对 MLFQ 仍执行完整行为验证，其他策略保留轻量活性检查。 */
static int
run_smoke(int policy)
{
  const int hints[WORKERS] = {4, 3, 2};
  const int weights[WORKERS] = {1024, 2048, 512};
  struct worker_result results[WORKERS];

  if(policy == SCHED_POLICY_MLFQ)
    return verify_mlfq();
  return run_barrier_workers(hints, weights, results);
}

static char *
policy_name(int policy)
{
  static char *names[] = {"rr", "fifo", "sjf", "stcf", "mlfq", "cfs"};
  if(policy < 0 || policy >= 6)
    return "unknown";
  return names[policy];
}

int
main(int argc, char **argv)
{
  struct sched_stats stats;
  int ok;

  if(sched_get_stats(&stats) < 0){
    printf("schedtest: unable to read policy\n");
    exit(1);
  }

  if(argc > 1 && strcmp(argv[1], "smoke") == 0){
    ok = run_smoke(stats.policy);
  } else {
    switch(stats.policy){
    case SCHED_POLICY_FIFO:
    case SCHED_POLICY_SJF:
      ok = verify_fifo_or_sjf(stats.policy);
      break;
    case SCHED_POLICY_STCF:
      ok = verify_stcf();
      break;
    case SCHED_POLICY_MLFQ:
      ok = verify_mlfq();
      break;
    case SCHED_POLICY_CFS:
      ok = verify_cfs();
      break;
    case SCHED_POLICY_RR:
    default:
      ok = verify_rr();
      break;
    }
  }

  if(ok < 0){
    printf("schedtest: FAIL policy=%s\n", policy_name(stats.policy));
    exit(1);
  }
  printf("schedtest: OK policy=%s\n", policy_name(stats.policy));
  exit(0);
}
