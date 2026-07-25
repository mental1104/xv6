#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/schedtrace_abi.h"
#include "user/user.h"

#define CHILDREN 2
#define OBSERVE_RETRIES 200

/** 子进程通过结果管道返回的一次读取结果。 */
struct reader_result {
  int pid;
  int bytes;
  char value;
};

static struct schedtrace_snapshot snapshot;
static int live_children[CHILDREN];

/**
 * terminate_children 终止并回收当前测试仍可能存活的子进程。
 *
 * 该函数仅用于失败清理；kill() 会让阻塞在 pipe read 的子进程离开睡眠，随后由
 * wait() 回收，避免失败用例把孤儿进程留给后续回归。
 */
static void
terminate_children(void)
{
  for(int i = 0; i < CHILDREN; i++)
    if(live_children[i] > 0)
      kill(live_children[i]);
  for(int i = 0; i < CHILDREN; i++)
    if(live_children[i] > 0)
      wait(0);
}

/**
 * fail 输出稳定失败原因，清理子进程并以非零状态结束测试。
 *
 * @param message 描述未满足的行为契约。
 */
static void
fail(char *message)
{
  printf("condvartest: FAIL: %s\n", message);
  terminate_children();
  exit(1);
}

/**
 * read_exact 从管道读取固定字节数。
 *
 * @param fd 可读文件描述符。
 * @param buffer 接收数据的缓冲区。
 * @param size 必须读取的字节数。
 * @return 读满返回 0；遇到 EOF 或错误返回 -1。
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
 * write_exact 向管道写入固定字节数。
 *
 * @param fd 可写文件描述符。
 * @param buffer 只读输入缓冲区，所有权仍归调用者。
 * @param size 必须写入的字节数。
 * @return 写满返回 0；管道关闭或写入失败返回 -1。
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
 * trace_start 为指定子进程建立新的调度轨迹会话。
 *
 * @param pids 需要观察的子进程 PID 数组。
 * @param count PID 数量，不能超过 schedtrace 过滤器容量。
 */
static void
trace_start(int pids[], int count)
{
  if(schedtrace(SCHEDTRACE_OP_RESET, 0, 0) < 0)
    fail("trace reset");
  for(int i = 0; i < count; i++)
    if(schedtrace(SCHEDTRACE_OP_WATCH_PID, 0, pids[i]) < 0)
      fail("trace watch pid");
  if(schedtrace(SCHEDTRACE_OP_START, 0, 0) < 0)
    fail("trace start");
}

/** 停止当前调度轨迹会话，并保留快照供后续读取。 */
static void
trace_stop(void)
{
  if(schedtrace(SCHEDTRACE_OP_STOP, 0, 0) < 0)
    fail("trace stop");
}

/** 读取当前完整 schedtrace 快照到静态缓冲。 */
static void
trace_read(void)
{
  if(schedtrace(SCHEDTRACE_OP_READ, &snapshot, SCHEDTRACE_MAX_EVENTS) < 0)
    fail("trace read");
  if(snapshot.dropped != 0)
    fail("trace events dropped");
}

/**
 * count_events 统计某进程满足类型与停止原因的调度事件。
 *
 * @param pid 目标子进程 PID。
 * @param event_type SCHEDTRACE_EVENT_*；传 0 表示不限制事件类型。
 * @param stop_reason SCHEDTRACE_REASON_*；传负数表示不限制停止原因。
 * @param after_seq 只统计序号严格大于该值的事件。
 * @return 匹配事件数量。
 */
static int
count_events(int pid, int event_type, int stop_reason, unsigned long after_seq)
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
last_sequence(void)
{
  unsigned long result = 0;

  for(int i = 0; i < snapshot.events; i++)
    if(snapshot.events_buffer[i].seq > result)
      result = snapshot.events_buffer[i].seq;
  return result;
}

/**
 * spawn_reader 创建一个由 gate 控制、随后读取共享 data pipe 的子进程。
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
spawn_reader(int data_read, int data_write,
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
  if(write_exact(ready_write, &token, 1) < 0)
    exit(1);
  close(ready_write);
  if(read_exact(gate_read, &token, 1) < 0)
    exit(1);
  close(gate_read);

  struct reader_result result;
  result.pid = getpid();
  result.bytes = read(data_read, &result.value, 1);
  close(data_read);
  if(write_exact(result_write, &result, sizeof(result)) < 0)
    exit(1);
  close(result_write);
  exit(result.bytes == 1 ? 0 : 1);
}

/**
 * wait_for_initial_sleeps 等待两个 reader 都因空 data pipe 再次睡眠。
 *
 * gate 的唤醒先让 reader 产生 RUN_START；随后 piperead() 检查空谓词并调用
 * sleep()，scheduler 记录 RUN_STOP/SLEEP。轮询依据状态事件而非人工抢时机。
 */
static void
wait_for_initial_sleeps(int pids[])
{
  for(int attempt = 0; attempt < OBSERVE_RETRIES; attempt++){
    trace_read();
    if(count_events(pids[0], SCHEDTRACE_EVENT_RUN_STOP,
                    SCHEDTRACE_REASON_SLEEP, 0) > 0 &&
       count_events(pids[1], SCHEDTRACE_EVENT_RUN_STOP,
                    SCHEDTRACE_REASON_SLEEP, 0) > 0)
      return;
    sleep(1);
  }
  fail("readers did not reach pipe sleep");
}

/**
 * wait_for_ineffective_wakeup 等待一次写入形成“两个 reader 运行、一个退出、一个重睡”。
 *
 * @param pids 两个 reader PID。
 * @return 被无效唤醒后重新睡眠的 reader 下标；超时直接失败。
 */
static int
wait_for_ineffective_wakeup(int pids[])
{
  for(int attempt = 0; attempt < OBSERVE_RETRIES; attempt++){
    int slept[CHILDREN];
    int exited[CHILDREN];

    trace_read();
    for(int i = 0; i < CHILDREN; i++){
      if(count_events(pids[i], SCHEDTRACE_EVENT_RUN_START, -1, 0) == 0)
        break;
      slept[i] = count_events(pids[i], SCHEDTRACE_EVENT_RUN_STOP,
                              SCHEDTRACE_REASON_SLEEP, 0) > 0;
      exited[i] = count_events(pids[i], SCHEDTRACE_EVENT_RUN_STOP,
                               SCHEDTRACE_REASON_EXIT, 0) > 0;
      if(i == CHILDREN - 1){
        if(slept[0] && exited[1] && !exited[0])
          return 0;
        if(slept[1] && exited[0] && !exited[1])
          return 1;
      }
    }
    sleep(1);
  }
  fail("wakeup did not produce one exit and one re-sleep");
  return -1;
}

/**
 * verify_preexisting_data 验证谓词已成立时 reader 不依赖历史 wakeup。
 *
 * 父进程先写入字节，再释放 reader。调度轨迹应只包含 gate 唤醒后的运行与退出，
 * 不应出现 data pipe 上的 RUN_STOP/SLEEP。
 */
static void
verify_preexisting_data(void)
{
  int data[2];
  int ready[2];
  int gate[2];
  int result_pipe[2];
  int status = 0;
  int pid;
  char token;
  struct reader_result result;

  memset(live_children, 0, sizeof(live_children));
  if(pipe(data) < 0 || pipe(ready) < 0 || pipe(gate) < 0 || pipe(result_pipe) < 0)
    fail("preexisting pipe setup");

  pid = spawn_reader(data[0], data[1], ready[0], ready[1],
                     gate[0], gate[1], result_pipe[0], result_pipe[1]);
  if(pid < 0)
    fail("preexisting fork");
  live_children[0] = pid;

  close(data[0]);
  close(ready[1]);
  close(gate[0]);
  close(result_pipe[1]);
  if(read_exact(ready[0], &token, 1) < 0)
    fail("preexisting ready");
  close(ready[0]);

  token = 'P';
  if(write_exact(data[1], &token, 1) < 0)
    fail("preexisting data write");
  trace_start(&pid, 1);
  token = 'g';
  if(write_exact(gate[1], &token, 1) < 0)
    fail("preexisting gate release");
  close(gate[1]);
  close(data[1]);

  if(wait(&status) != pid || status != 0)
    fail("preexisting child status");
  live_children[0] = 0;
  trace_stop();
  trace_read();
  if(read_exact(result_pipe[0], &result, sizeof(result)) < 0)
    fail("preexisting result");
  close(result_pipe[0]);
  if(result.pid != pid || result.bytes != 1 || result.value != 'P')
    fail("preexisting predicate result");
  if(count_events(pid, SCHEDTRACE_EVENT_RUN_STOP,
                  SCHEDTRACE_REASON_SLEEP, 0) != 0)
    fail("preexisting data unexpectedly slept");
  if(count_events(pid, SCHEDTRACE_EVENT_RUN_STOP,
                  SCHEDTRACE_REASON_EXIT, 0) == 0)
    fail("preexisting exit event missing");
}

/**
 * verify_wakeup_rechecks_predicate 验证 pipe 对 sleep/wakeup 的完整条件等待协议。
 *
 * 两个 reader 先稳定睡在同一 data channel。第一次只写一个字节时，wakeup() 会让
 * 两者都重新运行；一个 reader 消费字节并退出，另一个在重新持有 pipe lock 后发现
 * 谓词仍为假，再次进入 RUN_STOP/SLEEP。第二次写入才让剩余 reader 完成。
 */
static void
verify_wakeup_rechecks_predicate(void)
{
  int data[2];
  int ready[2];
  int gate[2];
  int result_pipe[2];
  int pids[CHILDREN];
  int statuses[CHILDREN];
  char ready_tokens[CHILDREN];
  char gate_tokens[CHILDREN] = {'g', 'g'};
  struct reader_result results[CHILDREN];

  memset(live_children, 0, sizeof(live_children));
  if(pipe(data) < 0 || pipe(ready) < 0 || pipe(gate) < 0 || pipe(result_pipe) < 0)
    fail("recheck pipe setup");

  for(int i = 0; i < CHILDREN; i++){
    pids[i] = spawn_reader(data[0], data[1], ready[0], ready[1],
                           gate[0], gate[1], result_pipe[0], result_pipe[1]);
    if(pids[i] < 0)
      fail("recheck fork");
    live_children[i] = pids[i];
  }

  close(data[0]);
  close(ready[1]);
  close(gate[0]);
  close(result_pipe[1]);
  if(read_exact(ready[0], ready_tokens, sizeof(ready_tokens)) < 0)
    fail("recheck readers ready");
  close(ready[0]);

  trace_start(pids, CHILDREN);
  if(write_exact(gate[1], gate_tokens, sizeof(gate_tokens)) < 0)
    fail("recheck gate release");
  close(gate[1]);
  wait_for_initial_sleeps(pids);
  trace_stop();

  // 清空 gate 阶段轨迹，只观察 data pipe 的一次状态改变和重检结果。
  trace_start(pids, CHILDREN);
  char first = 'A';
  if(write_exact(data[1], &first, 1) < 0)
    fail("recheck first write");
  int loser = wait_for_ineffective_wakeup(pids);
  unsigned long first_phase_end = last_sequence();

  char second = 'B';
  if(write_exact(data[1], &second, 1) < 0)
    fail("recheck second write");
  close(data[1]);

  for(int i = 0; i < CHILDREN; i++){
    int waited = wait(&statuses[i]);
    if(waited < 0 || statuses[i] != 0)
      fail("recheck child status");
    if(waited == pids[0])
      live_children[0] = 0;
    else if(waited == pids[1])
      live_children[1] = 0;
    else
      fail("recheck unexpected child");
  }
  trace_stop();
  trace_read();

  if(count_events(pids[loser], SCHEDTRACE_EVENT_RUN_START,
                  -1, first_phase_end) == 0)
    fail("re-slept reader was not started by second write");
  if(count_events(pids[loser], SCHEDTRACE_EVENT_RUN_STOP,
                  SCHEDTRACE_REASON_EXIT, first_phase_end) == 0)
    fail("re-slept reader did not exit after second write");

  if(read_exact(result_pipe[0], results, sizeof(results)) < 0)
    fail("recheck result read");
  close(result_pipe[0]);

  int saw_a = 0;
  int saw_b = 0;
  int saw_pid[CHILDREN] = {0, 0};
  for(int i = 0; i < CHILDREN; i++){
    if(results[i].bytes != 1)
      fail("reader continued after ineffective wakeup");
    if(results[i].value == 'A')
      saw_a++;
    else if(results[i].value == 'B')
      saw_b++;
    else
      fail("reader returned unexpected byte");
    if(results[i].pid == pids[0])
      saw_pid[0]++;
    else if(results[i].pid == pids[1])
      saw_pid[1]++;
    else
      fail("result pid not owned by scenario");
  }
  if(saw_a != 1 || saw_b != 1 || saw_pid[0] != 1 || saw_pid[1] != 1)
    fail("reader results were not one-to-one");
}

/**
 * main 执行 xv6 条件等待的 guest-first 回归。
 *
 * @return 所有谓词、睡眠、唤醒和重检断言成立时 exit(0)，否则 exit(1)。
 */
int
main(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  verify_preexisting_data();
  verify_wakeup_rechecks_predicate();
  printf("condvartest: OK\n");
  exit(0);
}
