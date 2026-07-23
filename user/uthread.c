#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/uthread.h"

#define PREEMPT_MIN_ITERATIONS 1000000ULL
#define PREEMPT_TIMEOUT_TICKS 20
#define LIFECYCLE_ROUNDS 3

static volatile uint64 preempt_progress[2];
static volatile uint64 preempt_result[2];
static volatile uint64 preempt_iterations[2];
static volatile int preempt_saw_peer[2];
static volatile int capacity_seen[UTHREAD_MAX_WORKERS];
static volatile int lifecycle_seen[LIFECYCLE_ROUNDS][UTHREAD_MAX_WORKERS];

/**
 * CPU 密集型抢占测试线程，不主动调用 thread_yield()。
 *
 * 每个线程至少执行一百万次运算，并继续运行到观察到另一线程已前进。这样首个
 * 线程不会在 timer tick 到达前退出，测试能够区分真正抢占和 thread_exit() 带来
 * 的顺序切换。20 tick 仅作为 alarm 失效时的失败上限，不参与正常调度。
 *
 * @param argument 取值为 0 或 1 的线程序号，通过整数到指针的转换传入。
 * @return 无；结果写入 preempt_* 全局数组后由 trampoline 自动退出。
 */
static void
preempt_worker(void *argument)
{
  uint64 id = (uint64)argument;
  uint64 peer = 1 - id;
  uint64 seed = 17 + id;
  uint64 iterations = 0;
  uint64 sum = 0;
  int start_tick = uptime();

  for(;;){
    sum += iterations + seed;
    iterations++;
    preempt_progress[id] = iterations;
    if(preempt_progress[peer] != 0)
      preempt_saw_peer[id] = 1;

    if(iterations >= PREEMPT_MIN_ITERATIONS && preempt_saw_peer[id])
      break;
    if((iterations & 0x3ffffULL) == 0 &&
       uptime() - start_tick >= PREEMPT_TIMEOUT_TICKS)
      break;
  }

  preempt_iterations[id] = iterations;
  preempt_result[id] = sum;
}

/**
 * 记录容量测试中对应工作线程已真正运行。
 *
 * @param argument 指向稳定存在的 int 下标，范围为 0 到 UTHREAD_MAX_WORKERS-1。
 * @return 无；只写入当前下标对应的独立标志位。
 */
static void
capacity_worker(void *argument)
{
  int index = *(int *)argument;
  capacity_seen[index] = 1;
}

/** 生命周期测试传给一个工作线程的轮次与槽位下标。 */
struct lifecycle_argument {
  int round;
  int index;
};

/**
 * 记录生命周期压力测试中某一轮、某一槽位对应的执行结果。
 *
 * @param argument 指向主线程栈上稳定存在的 lifecycle_argument 数组元素。
 * @return 无；只写入唯一的二维标志位，避免共享自增的数据竞争。
 */
static void
lifecycle_worker(void *argument)
{
  struct lifecycle_argument *item = argument;
  lifecycle_seen[item->round][item->index] = 1;
}

/**
 * 验证两个不主动 yield 的 CPU 密集线程会被 timer 交替抢占且计算上下文完整。
 *
 * @return 验收通过返回 0；创建、join、并发进展或寄存器相关计算异常返回 -1。
 */
static int
run_preempt_test(void)
{
  int tids[2];

  for(int i = 0; i < 2; i++){
    tids[i] = thread_create(preempt_worker, (void *)(uint64)i);
    if(tids[i] < 0)
      return -1;
  }
  for(int i = 0; i < 2; i++){
    if(thread_join(tids[i]) < 0)
      return -1;
  }

  for(uint64 id = 0; id < 2; id++){
    uint64 iterations = preempt_iterations[id];
    uint64 expected = iterations * (iterations - 1) / 2;
    expected += iterations * (17 + id);
    if(iterations < PREEMPT_MIN_ITERATIONS ||
       preempt_result[id] != expected || !preempt_saw_peer[id]){
      printf("uthread: preempt detail id=%d iterations=%d peer=%d result=%d expected=%d\n",
             id, iterations, preempt_saw_peer[id], preempt_result[id], expected);
      return -1;
    }
  }
  return 0;
}

/**
 * 验证 16 个工作线程可同时占用全部容量，第 17 次创建明确失败且已有线程完好。
 *
 * @return 容量、失败语义和执行结果全部正确返回 0，否则返回 -1；失败时打印
 *         精确阶段、线程编号和观察值，便于区分分配与回收错误。
 */
static int
run_capacity_test(void)
{
  int arguments[UTHREAD_MAX_WORKERS];
  int tids[UTHREAD_MAX_WORKERS];

  // xv6 用户 ELF 从虚拟地址 0 开始链接；首个容量工作函数也覆盖合法零地址入口回归。
  for(int i = 0; i < UTHREAD_MAX_WORKERS; i++){
    arguments[i] = i;
    tids[i] = thread_create(capacity_worker, &arguments[i]);
    if(tids[i] < 0){
      printf("uthread: capacity detail create index=%d tid=%d\n", i, tids[i]);
      return -1;
    }
  }

  int overflow_tid = thread_create(capacity_worker, &arguments[0]);
  if(overflow_tid != -1){
    printf("uthread: capacity detail overflow tid=%d\n", overflow_tid);
    return -1;
  }

  for(int i = 0; i < UTHREAD_MAX_WORKERS; i++){
    int join_result = thread_join(tids[i]);
    if(join_result < 0 || capacity_seen[i] != 1){
      printf("uthread: capacity detail join index=%d tid=%d result=%d seen=%d\n",
             i, tids[i], join_result, capacity_seen[i]);
      return -1;
    }
  }
  return 0;
}

/**
 * 连续三轮创建、运行、join 16 个线程，验证 ZOMBIE 槽位可稳定回收复用。
 *
 * @return 48 次线程执行均完成且每轮 join 成功返回 0，否则返回 -1。
 */
static int
run_lifecycle_test(void)
{
  struct lifecycle_argument arguments[UTHREAD_MAX_WORKERS];
  int tids[UTHREAD_MAX_WORKERS];

  for(int round = 0; round < LIFECYCLE_ROUNDS; round++){
    for(int i = 0; i < UTHREAD_MAX_WORKERS; i++){
      arguments[i].round = round;
      arguments[i].index = i;
      tids[i] = thread_create(lifecycle_worker, &arguments[i]);
      if(tids[i] < 0)
        return -1;
    }
    for(int i = 0; i < UTHREAD_MAX_WORKERS; i++){
      if(thread_join(tids[i]) < 0 || lifecycle_seen[round][i] != 1)
        return -1;
    }
  }
  return 0;
}

/**
 * 运行可抢占 M:1 用户线程的抢占、容量和生命周期验收场景。
 *
 * @param argc 命令行参数数量，本程序忽略额外参数。
 * @param argv 命令行参数数组，本程序忽略额外参数。
 * @return 通过 exit() 返回：全部通过为 0，初始化或任一阶段失败为 1。
 */
int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  if(thread_init() < 0 || thread_start() < 0){
    printf("uthread: initialization failed\n");
    exit(1);
  }

  if(run_preempt_test() < 0){
    printf("uthread: preempt FAILED\n");
    exit(1);
  }
  printf("uthread: preempt OK\n");

  if(run_capacity_test() < 0){
    printf("uthread: capacity FAILED\n");
    exit(1);
  }
  printf("uthread: capacity OK created=%d overflow=-1\n", UTHREAD_MAX_WORKERS);

  if(run_lifecycle_test() < 0){
    printf("uthread: lifecycle FAILED\n");
    exit(1);
  }
  printf("uthread: lifecycle OK rounds=%d completed=%d\n",
         LIFECYCLE_ROUNDS, LIFECYCLE_ROUNDS * UTHREAD_MAX_WORKERS);

  sigalarm(0, 0);
  printf("uthread: all tests OK\n");
  exit(0);
}
