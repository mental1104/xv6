#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define SOURCE_COUNT 2
#define SOURCE_A 0
#define SOURCE_B 1
#define SOURCE_A_EVENTS 2
#define SOURCE_B_EVENTS 1
#define TOTAL_EVENTS (SOURCE_A_EVENTS + SOURCE_B_EVENTS)
#define SLOW_HANDLER_TICKS 20
#define SLOW_LAG_MIN_TICKS 10

/** 一个由 pipe 传递、并由事件循环分派的完整教学事件。 */
struct event_message {
  int source;
  int sequence;
  int produced_tick;
};

/** 事件循环显式保存的单个事件源状态。 */
struct source_state {
  int fd;
  int source;
  int expected_sequence;
  int handled;
  int eof;
};

/** 输出稳定失败原因并以非零状态终止事件循环。 */
static void
fail(char *message)
{
  printf("eventloop: FAIL: %s\n", message);
  exit(1);
}

/** 断言一个事件循环不变量。 */
static void
check(int condition, char *message)
{
  if(!condition)
    fail(message);
}

/** 返回事件源的稳定公开名称。 */
static char *
source_name(int source)
{
  if(source == SOURCE_A)
    return "A";
  if(source == SOURCE_B)
    return "B";
  return "?";
}

/** 向事件源 pipe 原子写入一条固定大小事件。 */
static void
emit_event(int fd, int source, int sequence)
{
  struct event_message event = {
    .source = source,
    .sequence = sequence,
    .produced_tick = uptime(),
  };

  if(write(fd, &event, sizeof(event)) != sizeof(event))
    exit(1);
}

/**
 * 生成固定、可重复的多源事件负载。
 *
 * A 在第一个事件后保留较长间隔；B 会在慢处理器睡眠期间产生事件，从而形成
 * “事件已就绪但尚未被分派”的稳定观察窗口。
 */
static void
run_source(int source, int fd)
{
  if(source == SOURCE_A){
    sleep(2);
    emit_event(fd, source, 1);
    sleep(24);
    emit_event(fd, source, 2);
  } else if(source == SOURCE_B){
    sleep(6);
    emit_event(fd, source, 1);
  } else {
    exit(1);
  }

  close(fd);
  exit(0);
}

/** 创建一个先等待共同 gate、再产生事件的子进程。 */
static int
spawn_source(int source, int pipes[SOURCE_COUNT][2], int gate[2])
{
  int pid = fork();

  check(pid >= 0, "cannot fork event source");
  if(pid == 0){
    char token;

    close(gate[1]);
    for(int index = 0; index < SOURCE_COUNT; index++){
      close(pipes[index][0]);
      if(index != source)
        close(pipes[index][1]);
    }

    if(read(gate[0], &token, 1) != 1)
      exit(1);
    close(gate[0]);
    run_source(source, pipes[source][1]);
  }

  return pid;
}

/** 等待指定事件源子进程成功退出。 */
static void
wait_source(int pid)
{
  int status = 0;

  check(waitpid(pid, &status, 0) == pid, "waitpid returned unexpected source");
  check(status == 0, "event source exited with failure");
}

/** 根据显式状态表构造本轮仍需等待的 fd 与状态下标。 */
static int
build_active_set(struct source_state states[SOURCE_COUNT],
                 int fds[SOURCE_COUNT], int indexes[SOURCE_COUNT])
{
  int count = 0;

  for(int index = 0; index < SOURCE_COUNT; index++){
    if(states[index].eof)
      continue;
    fds[count] = states[index].fd;
    indexes[count] = index;
    count++;
  }
  return count;
}

/**
 * 分派一条已经从就绪 pipe 读取的事件，并推进显式来源状态。
 *
 * 状态更新严格校验来源与序号，能把“两个来源共用一份进度”或“遗漏一个事件”
 * 转化为确定失败，而不是只依赖输出顺序观察。
 */
static void
dispatch_event(struct source_state states[SOURCE_COUNT], int state_index,
               struct event_message *event, int slow_handler,
               int *total_handled, int *slow_source_lag)
{
  struct source_state *state = &states[state_index];
  int dispatch_tick = uptime();
  int lag = dispatch_tick - event->produced_tick;

  check(event->source == state->source, "event delivered to wrong state owner");
  check(event->sequence == state->expected_sequence,
        "event sequence was omitted or state was shared");

  printf("EVENT ready source=%s sequence=%d lag=%d\n",
         source_name(event->source), event->sequence, lag);
  printf("DISPATCH source=%s sequence=%d\n",
         source_name(event->source), event->sequence);

  if(slow_handler && event->source == SOURCE_A && event->sequence == 1){
    printf("HANDLER slow begin source=A ticks=%d\n", SLOW_HANDLER_TICKS);
    sleep(SLOW_HANDLER_TICKS);
    printf("HANDLER slow end source=A\n");
  }

  state->handled++;
  state->expected_sequence++;
  (*total_handled)++;
  if(slow_handler && event->source == SOURCE_B)
    *slow_source_lag = lag;

  printf("STATE a=%d b=%d total=%d\n",
         states[SOURCE_A].handled, states[SOURCE_B].handled, *total_handled);
}

/** 执行一次完整的多源事件循环实验。 */
static int
run_event_loop(int slow_handler)
{
  int pipes[SOURCE_COUNT][2];
  int gate[2];
  int source_pids[SOURCE_COUNT];
  int initial_fds[SOURCE_COUNT];
  int total_handled = 0;
  int slow_source_lag = -1;
  int open_sources = SOURCE_COUNT;
  struct source_state states[SOURCE_COUNT];

  for(int index = 0; index < SOURCE_COUNT; index++)
    check(pipe(pipes[index]) == 0, "cannot create source pipe");
  check(pipe(gate) == 0, "cannot create source gate");

  for(int index = 0; index < SOURCE_COUNT; index++)
    source_pids[index] = spawn_source(index, pipes, gate);

  close(gate[0]);
  for(int index = 0; index < SOURCE_COUNT; index++){
    close(pipes[index][1]);
    states[index].fd = pipes[index][0];
    states[index].source = index;
    states[index].expected_sequence = 1;
    states[index].handled = 0;
    states[index].eof = 0;
    initial_fds[index] = pipes[index][0];
  }

  // 子进程仍被 gate 阻塞，因此这个零结果是确定的非阻塞快照，不依赖调度运气。
  check(pollread(initial_fds, SOURCE_COUNT, 0) == 0,
        "empty nonblocking snapshot reported readiness");
  printf("EVENTLOOP snapshot ready=0\n");

  check(write(gate[1], "gg", SOURCE_COUNT) == SOURCE_COUNT,
        "cannot release event sources");
  close(gate[1]);

  while(open_sources > 0){
    int active_fds[SOURCE_COUNT];
    int active_indexes[SOURCE_COUNT];
    int active_count = build_active_set(states, active_fds, active_indexes);
    int ready;

    check(active_count == open_sources, "active source count diverged");
    ready = pollread(active_fds, active_count, 1);
    check(ready > 0, "blocking readiness wait failed");

    for(int slot = 0; slot < active_count; slot++){
      struct source_state *state;
      struct event_message event;
      int count;

      if((ready & (1 << slot)) == 0)
        continue;
      state = &states[active_indexes[slot]];
      count = read(state->fd, &event, sizeof(event));
      if(count == 0){
        state->eof = 1;
        open_sources--;
        close(state->fd);
        printf("EVENT eof source=%s\n", source_name(state->source));
        continue;
      }
      check(count == sizeof(event), "pipe returned a partial event");
      dispatch_event(states, active_indexes[slot], &event, slow_handler,
                     &total_handled, &slow_source_lag);
    }
  }

  for(int index = 0; index < SOURCE_COUNT; index++)
    wait_source(source_pids[index]);

  check(states[SOURCE_A].handled == SOURCE_A_EVENTS,
        "source A final event count is wrong");
  check(states[SOURCE_B].handled == SOURCE_B_EVENTS,
        "source B final event count is wrong");
  check(total_handled == TOTAL_EVENTS, "global event count is wrong");
  printf("ORACLE state-complete PASS a=%d b=%d total=%d\n",
         states[SOURCE_A].handled, states[SOURCE_B].handled, total_handled);

  if(slow_handler){
    check(slow_source_lag >= SLOW_LAG_MIN_TICKS,
          "slow handler did not delay unrelated ready event");
    printf("ORACLE slow-handler PASS source=B lag=%d\n", slow_source_lag);
  } else {
    printf("ORACLE slow-handler SKIP\n");
  }

  printf("EVENTLOOP done mode=%s\n", slow_handler ? "slow" : "fast");
  return 0;
}

/** 打印事件循环教学程序的参数格式。 */
static void
usage(void)
{
  fprintf(2, "Usage: eventloop [--slow]\n");
}

/** 选择普通事件循环或故意阻塞 handler 的反例模式。 */
int
main(int argc, char *argv[])
{
  int slow_handler = 0;

  if(argc == 2 && strcmp(argv[1], "--slow") == 0){
    slow_handler = 1;
  } else if(argc != 1){
    usage();
    exit(2);
  }

  exit(run_event_loop(slow_handler));
}
