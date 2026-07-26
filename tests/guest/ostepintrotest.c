#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/riscv.h"
#include "kernel/fcntl.h"
#include "kernel/schedstat.h"
#include "kernel/schedtrace_abi.h"
#include "user/user.h"

// 调度轨迹快照较大，放在 BSS 中避免占用 xv6 单页用户栈。
static struct schedtrace_snapshot trace_snapshot;

struct memory_observation {
  uint64 address;
  int before;
  int after;
};

/**
 * 输出稳定失败原因并以非零状态终止当前测试进程。
 *
 * @param message 描述未满足的不变量或失败步骤。
 */
static void
fail(char *message)
{
  printf("ostepintrotest: FAIL: %s\n", message);
  exit(1);
}

/**
 * 断言一个测试条件；条件不成立时立即终止测试。
 *
 * @param condition 非零表示断言成立。
 * @param message 断言失败时输出的诊断文本。
 */
static void
check(int condition, char *message)
{
  if(!condition)
    fail(message);
}

/** 执行一段不可被编译器消除的 CPU 工作，等待 timer tick 推进。 */
static void
burn(void)
{
  volatile unsigned long value = 1;
  for(int i = 0; i < 100000; i++)
    value = value * 1664525 + 1013904223;
}

/**
 * 让当前进程实际消耗指定数量的调度 runtime tick。
 *
 * @param ticks 目标 runtime tick 数，必须为正数。
 */
static void
consume_runtime_ticks(int ticks)
{
  struct sched_stats start;
  struct sched_stats current;

  check(ticks > 0, "runtime tick target is not positive");
  check(sched_get_stats(&start) == 0, "cannot read initial scheduler stats");
  do {
    burn();
    check(sched_get_stats(&current) == 0, "cannot read current scheduler stats");
  } while(current.runtime_ticks - start.runtime_ticks < (unsigned long)ticks);
}

/**
 * 创建一个先等待 gate、再执行 CPU 工作的子进程。
 *
 * @param ticks 子进程需要消耗的 runtime tick 数。
 * @param release_fd 返回父进程用于释放子进程的 pipe 写端。
 * @return 新建子进程 PID；基础设施失败时直接终止测试。
 */
static int
start_cpu_worker(int ticks, int *release_fd)
{
  int ready[2];
  int gate[2];
  int pid;
  char token;

  check(pipe(ready) == 0, "cannot create ready pipe");
  check(pipe(gate) == 0, "cannot create worker gate");
  pid = fork();
  check(pid >= 0, "cannot fork CPU worker");
  if(pid == 0){
    close(ready[0]);
    close(gate[1]);
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
  check(read(ready[0], &token, 1) == 1, "CPU worker did not become ready");
  close(ready[0]);
  *release_fd = gate[1];
  return pid;
}

/**
 * 释放一个等待 gate 的 CPU worker。
 *
 * @param release_fd start_cpu_worker() 返回的 pipe 写端；本函数会关闭它。
 */
static void
release_cpu_worker(int release_fd)
{
  check(write(release_fd, "x", 1) == 1, "cannot release CPU worker");
  close(release_fd);
}

/**
 * 等待指定子进程成功退出，确保失败不会被另一个子进程的状态掩盖。
 *
 * @param pid 需要回收的直接子进程 PID。
 */
static void
wait_successfully(int pid)
{
  int status = 0;
  check(waitpid(pid, &status, 0) == pid, "waitpid returned unexpected child");
  check(status == 0, "child process exited with failure");
}

/**
 * 返回整数位图中置位的数量。
 *
 * @param value 需要统计的 CPU 位图。
 * @return 置位数量。
 */
static int
count_bits(uint value)
{
  int count = 0;
  while(value != 0){
    count += value & 1;
    value >>= 1;
  }
  return count;
}

/**
 * 验证两个 CPU-bound 进程都被调度；单核配置还必须观察到分时复用。
 *
 * 测试不要求输出严格交替，也不把当前调度策略当作操作系统通用定义。
 */
static void
test_cpu_virtualization(void)
{
  int first_pid;
  int second_pid;
  int first_release;
  int second_release;
  int first_starts = 0;
  int first_stops = 0;
  int second_starts = 0;
  int second_stops = 0;
  uint first_cpu_mask = 0;
  uint second_cpu_mask = 0;

  check(schedtrace(SCHEDTRACE_OP_RESET, 0, 0) == 0, "cannot reset schedtrace");
  first_pid = start_cpu_worker(4, &first_release);
  second_pid = start_cpu_worker(4, &second_release);
  check(schedtrace(SCHEDTRACE_OP_WATCH_PID, 0, first_pid) == 0,
        "cannot watch first CPU worker");
  check(schedtrace(SCHEDTRACE_OP_WATCH_PID, 0, second_pid) == 0,
        "cannot watch second CPU worker");
  check(schedtrace(SCHEDTRACE_OP_START, 0, 0) == 0, "cannot start schedtrace");

  release_cpu_worker(first_release);
  release_cpu_worker(second_release);
  wait_successfully(first_pid);
  wait_successfully(second_pid);

  check(schedtrace(SCHEDTRACE_OP_STOP, 0, 0) == 0, "cannot stop schedtrace");
  check(schedtrace(SCHEDTRACE_OP_READ, &trace_snapshot,
                   SCHEDTRACE_MAX_EVENTS) == 0,
        "cannot read schedtrace snapshot");
  check(trace_snapshot.version == SCHEDTRACE_VERSION,
        "schedtrace version mismatch");
  check(trace_snapshot.cpus == XV6_CPUS, "schedtrace CPU count mismatch");
  check(trace_snapshot.active == 0, "schedtrace remained active after stop");
  check(trace_snapshot.events > 0, "schedtrace recorded no events");
  check(trace_snapshot.dropped == 0, "schedtrace dropped experiment events");

  for(int i = 0; i < trace_snapshot.events; i++){
    struct schedtrace_event *event = &trace_snapshot.events_buffer[i];
    check(event->pid == first_pid || event->pid == second_pid,
          "unwatched process leaked into schedtrace");
    check(event->cpu_id >= 0 && event->cpu_id < NCPU,
          "schedtrace event contains invalid CPU id");

    if(event->pid == first_pid){
      first_cpu_mask |= 1U << event->cpu_id;
      if(event->event_type == SCHEDTRACE_EVENT_RUN_START)
        first_starts++;
      else if(event->event_type == SCHEDTRACE_EVENT_RUN_STOP)
        first_stops++;
    } else {
      second_cpu_mask |= 1U << event->cpu_id;
      if(event->event_type == SCHEDTRACE_EVENT_RUN_START)
        second_starts++;
      else if(event->event_type == SCHEDTRACE_EVENT_RUN_STOP)
        second_stops++;
    }
  }

  check(first_starts > 0 && second_starts > 0,
        "one CPU worker was never scheduled");
  check(first_starts == first_stops && second_starts == second_stops,
        "RUN_START and RUN_STOP events are not paired");

  // 单核时两个 CPU-bound worker 必须共享同一 CPU，并至少各经历一次重新调度。
  if(trace_snapshot.cpus == 1){
    check(count_bits(first_cpu_mask | second_cpu_mask) == 1,
          "single-core workers used more than one CPU");
    check(first_starts > 1 && second_starts > 1,
          "single-core workers were not time-multiplexed");
  }

  printf("OSTEP CPU cpus=%d first_starts=%d second_starts=%d shared_cpu=%d\n",
         trace_snapshot.cpus, first_starts, second_starts,
         (first_cpu_mask & second_cpu_mask) != 0);
}

/** 验证 fork 后相同虚拟地址的写入彼此隔离，并回收实验页。 */
static void
test_memory_virtualization(void)
{
  int report[2];
  int status = 0;
  int pid;
  int parent_after;
  int observation_size = sizeof(struct memory_observation);
  int *value;
  char *page;
  struct memory_observation observation;

  page = sbrk(PGSIZE);
  check(page != (char *)-1, "cannot allocate memory experiment page");
  value = (int *)page;
  *value = 7;
  check(pipe(report) == 0, "cannot create memory report pipe");

  pid = fork();
  check(pid >= 0, "cannot fork memory experiment child");
  if(pid == 0){
    close(report[0]);
    observation.address = (uint64)value;
    observation.before = *value;
    *value = 99;
    observation.after = *value;
    if(write(report[1], &observation, observation_size) != observation_size)
      exit(1);
    close(report[1]);
    exit(0);
  }

  close(report[1]);
  check(read(report[0], &observation, observation_size) == observation_size,
        "cannot read child memory observation");
  close(report[0]);
  check(waitpid(pid, &status, 0) == pid, "cannot reap memory experiment child");
  check(status == 0, "memory experiment child failed");
  check(observation.address == (uint64)value,
        "fork changed the observed virtual address");
  check(observation.before == 7 && observation.after == 99,
        "child did not update its private view");
  parent_after = *value;
  check(parent_after == 7, "child write changed the parent value");
  check(sbrk(-PGSIZE) != (char *)-1, "cannot release memory experiment page");

  printf("OSTEP MEMORY same_va=1 parent=%d child=%d isolated=1\n",
         parent_after, observation.after);
}

/**
 * 判断两个字节范围是否完全相等。
 *
 * @param left 第一个至少包含 length 字节的缓冲区。
 * @param right 第二个至少包含 length 字节的缓冲区。
 * @param length 需要比较的字节数，必须非负。
 * @return 全部字节相等返回 1，否则返回 0。
 */
static int
bytes_equal(char *left, const char *right, int length)
{
  for(int i = 0; i < length; i++)
    if(left[i] != right[i])
      return 0;
  return 1;
}

/**
 * 读取普通文件直至 EOF 或缓冲区已满。
 *
 * @param path 要读取的文件路径。
 * @param buffer 接收文件内容的可写缓冲区。
 * @param capacity 缓冲区容量，单位为字节。
 * @return 实际读取字节数；打开或读取失败时直接终止测试。
 */
static int
read_file(char *path, char *buffer, int capacity)
{
  int fd;
  int total = 0;

  fd = open(path, O_RDONLY);
  check(fd >= 0, "cannot open experiment file for reading");
  while(total < capacity){
    int count = read(fd, buffer + total, capacity - total);
    check(count >= 0, "experiment file read failed");
    if(count == 0)
      break;
    total += count;
  }
  check(close(fd) == 0, "cannot close experiment input file");
  return total;
}

/**
 * 验证 write() 只服从缓冲区与显式长度，并用多写 NUL 的反例击穿字符串直觉。
 *
 * 本测试只证明同一次运行中的可见字节语义，不声称数据已经经受掉电持久化。
 */
static void
test_io_abstraction(void)
{
  static const char payload[] = "hello world\n";
  char exact_buffer[32];
  char nul_buffer[32];
  char *exact_path = "ostepio-exact";
  char *nul_path = "ostepio-nul";
  int exact_bytes = sizeof(payload) - 1;
  int payload_storage_bytes = sizeof(payload);
  int fd;

  unlink(exact_path);
  unlink(nul_path);

  fd = open(exact_path, O_CREATE | O_TRUNC | O_WRONLY);
  check(fd >= 0, "cannot create exact-length experiment file");
  check(write(fd, payload, exact_bytes) == exact_bytes,
        "exact-length write returned a short count");
  check(close(fd) == 0, "cannot close exact-length experiment file");
  check(read_file(exact_path, exact_buffer, sizeof(exact_buffer)) == exact_bytes,
        "exact-length file size mismatch");
  check(bytes_equal(exact_buffer, payload, exact_bytes),
        "exact-length file payload mismatch");

  // sizeof(payload) 包含字符串末尾 NUL；内核不会替调用者推断逻辑字符串长度。
  fd = open(nul_path, O_CREATE | O_TRUNC | O_WRONLY);
  check(fd >= 0, "cannot create NUL counterexample file");
  check(write(fd, payload, payload_storage_bytes) == payload_storage_bytes,
        "NUL counterexample write returned a short count");
  check(close(fd) == 0, "cannot close NUL counterexample file");
  check(read_file(nul_path, nul_buffer, sizeof(nul_buffer)) == payload_storage_bytes,
        "NUL counterexample file size mismatch");
  check(bytes_equal(nul_buffer, payload, exact_bytes),
        "NUL counterexample payload prefix mismatch");
  check(nul_buffer[exact_bytes] == 0,
        "explicit sizeof(payload) did not write the trailing NUL");

  check(unlink(exact_path) == 0, "cannot remove exact-length experiment file");
  check(unlink(nul_path) == 0, "cannot remove NUL counterexample file");
  printf("OSTEP IO payload=%d exact_file=%d sizeof_file=%d trailing_nul=1\n",
         exact_bytes, exact_bytes, payload_storage_bytes);
}

/**
 * 依次执行 CPU、内存与 I/O 三条可观察闭环。
 *
 * @return 所有断言成立时以 exit(0) 结束；任一失败由 fail() 以 exit(1) 结束。
 */
int
main(void)
{
  test_cpu_virtualization();
  test_memory_virtualization();
  test_io_abstraction();
  printf("ostepintrotest: OK\n");
  exit(0);
}
