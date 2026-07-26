#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/disktrace_abi.h"
#include "user/user.h"
#include "user/disksched_model.h"

static struct disktrace_snapshot snapshot;
static char payload[BSIZE];

/** 打印稳定失败原因并以非零状态退出。 */
static void
fail(char *message)
{
  printf("diskschedtest: FAIL: %s\n", message);
  exit(1);
}

/** 校验结果顺序、请求数和模型成本。 */
static void
expect_model(struct disksched_result *result, const uint *order,
             int count, uint64 cost, char *label)
{
  if(result->count != count || result->cost != cost){
    printf("DISKMODEL mismatch policy=%s count=%d cost=%l\n",
           label, result->count, result->cost);
    fail("model header");
  }
  for(int i = 0; i < count; i++)
    if(result->order[i] != order[i])
      fail("model order");

  printf("DISKMODEL oracle policy=%s order=", label);
  for(int i = 0; i < count; i++){
    if(i != 0)
      printf(",");
    printf("%d", result->order[i]);
  }
  printf(" cost=%l unit=logical-block-distance\n", result->cost);
}

/**
 * verify_scheduling_model 比较同一请求集的 FCFS、SSTF 和向上 SCAN。
 *
 * 这些断言只验证逻辑块距离模型。SSTF 的较低成本是反例：它不证明 QEMU 完成
 * 更快，也不提供持续到达负载下的公平性。
 */
static void
verify_scheduling_model(void)
{
  static const uint requests[] = {98, 183, 37, 122, 14, 124, 65, 67};
  static const uint fcfs_order[] = {98, 183, 37, 122, 14, 124, 65, 67};
  static const uint sstf_order[] = {65, 67, 37, 14, 98, 122, 124, 183};
  static const uint scan_order[] = {65, 67, 98, 122, 124, 183, 37, 14};
  struct disksched_result fcfs;
  struct disksched_result sstf;
  struct disksched_result scan;

  if(disksched_model(DISKSCHED_POLICY_FCFS, 53, 199, requests, 8, &fcfs) < 0 ||
     disksched_model(DISKSCHED_POLICY_SSTF, 53, 199, requests, 8, &sstf) < 0 ||
     disksched_model(DISKSCHED_POLICY_SCAN, 53, 199, requests, 8, &scan) < 0)
    fail("model execution");

  expect_model(&fcfs, fcfs_order, 8, 640, "fcfs");
  expect_model(&sstf, sstf_order, 8, 236, "sstf");
  expect_model(&scan, scan_order, 8, 331, "scan");
  if(sstf.cost >= fcfs.cost || scan.cost >= fcfs.cost)
    fail("model comparison");
}

/** 确认非法边界被拒绝，并且失败不会部分覆盖调用者结果。 */
static void
verify_model_error_rollback(void)
{
  uint invalid_requests[] = {10, 200};
  struct disksched_result result;

  memset(&result, 0, sizeof(result));
  result.count = 7;
  result.cost = 99;
  result.order[0] = 123;
  if(disksched_model(DISKSCHED_POLICY_SCAN, 53, 199,
                     invalid_requests, 2, &result) >= 0)
    fail("invalid model request accepted");
  if(result.count != 7 || result.cost != 99 || result.order[0] != 123)
    fail("invalid model changed result");
}

/** 执行一个会触发日志提交和 virtio 写请求的最小文件工作负载。 */
static void
run_file_workload(void)
{
  int payload_size = sizeof(payload);
  int fd;

  for(int i = 0; i < payload_size; i++)
    payload[i] = (char)(i & 0x7f);
  fd = open("disktrace.tmp", O_CREATE | O_TRUNC | O_RDWR);
  if(fd < 0)
    fail("open workload file");
  if(write(fd, payload, payload_size) != payload_size)
    fail("write workload file");
  if(close(fd) < 0)
    fail("close workload file");
}

/** 读取完整驱动轨迹快照。 */
static void
read_trace(void)
{
  if(disktrace(DISKTRACE_OP_READ, &snapshot, DISKTRACE_MAX_EVENTS) < 0)
    fail("read trace");
}

/**
 * find_complete_request 寻找一个按顺序经历四个驱动可见阶段的请求。
 *
 * @return 找到时返回 request_id，否则返回 0。
 */
static uint64
find_complete_request(void)
{
  for(int i = 0; i < snapshot.events; i++){
    struct disktrace_event *submit = &snapshot.events_buffer[i];
    int expected = DISKTRACE_STAGE_QUEUED;

    if(submit->stage != DISKTRACE_STAGE_SUBMIT)
      continue;
    for(int j = i + 1; j < snapshot.events; j++){
      struct disktrace_event *event = &snapshot.events_buffer[j];
      if(event->request_id != submit->request_id)
        continue;
      if(event->blockno != submit->blockno || event->write != submit->write)
        fail("request identity changed");
      if(event->stage != expected)
        fail("request stage order");
      expected++;
      if(expected > DISKTRACE_STAGE_RETURN){
        printf("DISKTRACE request=%l block=%l op=%s stages=submit,queued,complete,return\n",
               submit->request_id, submit->blockno,
               submit->write ? "write" : "read");
        return submit->request_id;
      }
    }
  }
  return 0;
}

/** 校验事件序号、阶段枚举和固定容量头部。 */
static void
verify_trace_shape(void)
{
  uint64 previous_seq = 0;

  if(snapshot.version != DISKTRACE_VERSION || snapshot.events <= 0 ||
     snapshot.capacity != DISKTRACE_MAX_EVENTS || snapshot.active != 0 ||
     snapshot.dropped != 0)
    fail("trace snapshot header");
  for(int i = 0; i < snapshot.events; i++){
    struct disktrace_event *event = &snapshot.events_buffer[i];
    if(event->seq <= previous_seq)
      fail("trace sequence not increasing");
    previous_seq = event->seq;
    if(event->stage < DISKTRACE_STAGE_SUBMIT ||
       event->stage > DISKTRACE_STAGE_RETURN)
      fail("invalid trace stage");
  }
}

/** 确认默认关闭时真实文件 I/O 不会产生观察事件。 */
static void
verify_trace_default_off(void)
{
  unlink("disktrace.tmp");
  if(disktrace(DISKTRACE_OP_RESET, 0, 0) < 0)
    fail("reset default off");
  run_file_workload();
  read_trace();
  unlink("disktrace.tmp");
  if(snapshot.events != 0 || snapshot.active != 0)
    fail("default off recorded events");
}

/** 执行一次 reset/start/workload/stop/read，并返回一个完整请求编号。 */
static uint64
trace_one_workload(void)
{
  uint64 request_id;

  unlink("disktrace.tmp");
  if(disktrace(DISKTRACE_OP_RESET, 0, 0) < 0 ||
     disktrace(DISKTRACE_OP_START, 0, 0) < 0)
    fail("start trace session");
  run_file_workload();
  if(disktrace(DISKTRACE_OP_STOP, 0, 0) < 0)
    fail("stop trace session");
  read_trace();
  unlink("disktrace.tmp");

  verify_trace_shape();
  request_id = find_complete_request();
  if(request_id == 0)
    fail("no complete request lifecycle");
  return request_id;
}

/** 确认 reset 清空旧事件，但全局请求身份不会复用。 */
static void
verify_trace_repeatability(void)
{
  uint64 first = trace_one_workload();
  uint64 second = trace_one_workload();

  if(first == second)
    fail("request id reused across sessions");
  printf("DISKTRACE repeat reset_cleared=1 request_id_unique=1\n");
}

/** 覆盖非法 operation 和超出 ABI 上限的读取容量。 */
static void
verify_trace_invalid_inputs(void)
{
  if(disktrace(9999, 0, 0) >= 0)
    fail("invalid trace op accepted");
  if(disktrace(DISKTRACE_OP_READ, &snapshot,
               DISKTRACE_MAX_EVENTS + 1) >= 0)
    fail("oversized trace read accepted");
}

/**
 * main 验证实际 virtio 驱动边界和独立教学模型，两类证据不得互相替代。
 */
int
main(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  verify_scheduling_model();
  verify_model_error_rollback();
  verify_trace_default_off();
  verify_trace_repeatability();
  verify_trace_invalid_inputs();
  printf("DISKTRACE boundary=driver-visible qemu-device-order=opaque mechanical-seek=unobserved\n");
  printf("diskschedtest: OK\n");
  exit(0);
}
