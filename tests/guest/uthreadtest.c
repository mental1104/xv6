#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/concurrencylab.h"
#include "user/user.h"
#include "user/paths.h"
#include "tests/guest/testlib.h"

#define OUTPUT_SIZE 4096
#define CONCURRENCY_ROUNDS 2

/**
 * 执行可抢占 uthread 用户程序，并验证线程 API 与调度生命周期闭环。
 *
 * @return 完整输出和退出状态均符合预期返回 0，否则返回 -1。
 */
static int
run_uthread_regression(void)
{
  char *argv[] = {XV6_USR_BIN_PATH("uthread"), 0};
  char *output = malloc(OUTPUT_SIZE);
  int status;

  if(output == 0){
    printf("uthreadtest: malloc failed\n");
    return -1;
  }
  if(xv6_test_run_capture(argv, 0, output, OUTPUT_SIZE, &status) < 0){
    printf("uthreadtest: capture infrastructure failed\n");
    free(output);
    return -1;
  }

  if(status != 0 ||
     !xv6_test_contains(output, "uthread: shared-address OK same-pid=1 shared=43\n") ||
     !xv6_test_contains(output, "uthread: argument-lifetime OK observed=42 ownership=borrowed\n") ||
     !xv6_test_contains(output, "uthread: join-failures OK self=-1 duplicate=-1 invalid=-1\n") ||
     !xv6_test_contains(output, "uthread: stack-context OK distinct=1 preserved=2\n") ||
     !xv6_test_contains(output, "uthread: preempt OK\n") ||
     !xv6_test_contains(output, "uthread: capacity OK created=16 overflow=-1\n") ||
     !xv6_test_contains(output, "uthread: lifecycle OK rounds=3 completed=48\n") ||
     !xv6_test_contains(output, "uthread: all tests OK\n")){
    printf("uthreadtest: unexpected scheduling result status=%d\n%s", status, output);
    free(output);
    return -1;
  }

  free(output);
  printf("uthreadtest: preemptive runtime OK\n");
  return 0;
}

/** 读取恰好 n 个字节，避免 pipe 短读把不完整轨迹误判为有效结果。 */
static int
read_full(int fd, void *buffer, int n)
{
  int offset = 0;

  while(offset < n){
    int received = read(fd, (char *)buffer + offset, n - offset);
    if(received <= 0)
      return -1;
    offset += received;
  }
  return 0;
}

/** 写入恰好 n 个字节，保证子进程的结构化轨迹完整交给父进程。 */
static int
write_full(int fd, void *buffer, int n)
{
  int offset = 0;

  while(offset < n){
    int written = write(fd, (char *)buffer + offset, n - offset);
    if(written <= 0)
      return -1;
    offset += written;
  }
  return 0;
}

/**
 * 创建一个只执行指定实验角色的子进程。
 *
 * @param session 活动教学会话编号。
 * @param role 唯一角色 0 或 1。
 * @param pid_out 接收子进程 PID。
 * @param read_fd_out 接收父进程读取结构化轨迹的 pipe 端点。
 * @return 创建完成返回 0；pipe 或 fork 失败返回 -1。
 */
static int
start_worker(int session, int role, int *pid_out, int *read_fd_out)
{
  int fds[2];
  int pid;

  if(pipe(fds) < 0)
    return -1;
  pid = fork();
  if(pid < 0){
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if(pid == 0){
    struct concurrencylab_worker_snapshot worker;

    close(fds[0]);
    if(concurrencylab(CONCURRENCYLAB_OP_RUN, session, role, &worker) < 0 ||
       write_full(fds[1], &worker, sizeof(worker)) < 0){
      close(fds[1]);
      exit(1);
    }
    close(fds[1]);
    exit(0);
  }

  close(fds[1]);
  *pid_out = pid;
  *read_fd_out = fds[0];
  return 0;
}

/**
 * 在父进程错误路径上关闭会话并回收已经创建的工作进程。
 *
 * @param session 需要取消的活动会话编号。
 * @param pids 已创建的 PID 数组。
 * @param read_fds 父进程持有的 pipe 读取端数组。
 * @param count 数组中有效元素数量。
 */
static void
abort_workers(int session, int *pids, int *read_fds, int count)
{
  concurrencylab(CONCURRENCYLAB_OP_CLOSE, session, 0, 0);
  for(int i = 0; i < count; i++){
    if(read_fds[i] >= 0)
      close(read_fds[i]);
    if(pids[i] > 0)
      kill(pids[i]);
  }
  for(int i = 0; i < count; i++){
    if(pids[i] > 0)
      waitpid(pids[i], 0, 0);
  }
}

/** 判断子进程返回的参与者轨迹是否与内核会话快照完全一致。 */
static int
worker_matches(struct concurrencylab_worker_snapshot *left,
               struct concurrencylab_worker_snapshot *right)
{
  return left->role == right->role &&
         left->observed == right->observed &&
         left->written == right->written &&
         left->read_order == right->read_order &&
         left->write_order == right->write_order &&
         left->read_cpu == right->read_cpu &&
         left->write_cpu == right->write_cpu;
}

/**
 * 验证两种模式共用的会话、角色、CPU 范围和结果传递不变量。
 *
 * @return 所有公共不变量成立返回 0，否则打印具体字段并返回 -1。
 */
static int
validate_common(int session, int mode,
                struct concurrencylab_worker_snapshot *workers,
                struct concurrencylab_snapshot *snapshot)
{
  if(snapshot->session != session || snapshot->mode != mode ||
     snapshot->configured_cpus != XV6_CPUS || snapshot->active != 1 ||
     snapshot->completed != CONCURRENCYLAB_PARTICIPANTS){
    printf("uthreadtest: common state session=%d/%d mode=%d/%d cpus=%d/%d active=%d completed=%d\n",
           snapshot->session, session, snapshot->mode, mode,
           snapshot->configured_cpus, XV6_CPUS, snapshot->active,
           snapshot->completed);
    return -1;
  }

  for(int role = 0; role < CONCURRENCYLAB_PARTICIPANTS; role++){
    if(workers[role].role != role ||
       !worker_matches(&workers[role], &snapshot->workers[role]) ||
       workers[role].read_cpu < 0 || workers[role].read_cpu >= XV6_CPUS ||
       workers[role].write_cpu < 0 || workers[role].write_cpu >= XV6_CPUS){
      printf("uthreadtest: worker mismatch role=%d child_role=%d read_cpu=%d write_cpu=%d\n",
             role, workers[role].role,
             workers[role].read_cpu, workers[role].write_cpu);
      return -1;
    }
    if(XV6_CPUS == 1 &&
       (workers[role].read_cpu != 0 || workers[role].write_cpu != 0)){
      printf("uthreadtest: single cpu mismatch role=%d read_cpu=%d write_cpu=%d\n",
             role, workers[role].read_cpu, workers[role].write_cpu);
      return -1;
    }
  }
  return 0;
}

/**
 * 验证 RACY 模式固定两个读取先发生、两个写入都覆盖为 1 的 lost update。
 *
 * @return 竞争窗口和最终计数器均符合实验契约返回 0，否则返回 -1。
 */
static int
validate_racy(struct concurrencylab_worker_snapshot *workers,
              struct concurrencylab_snapshot *snapshot)
{
  int read_sum = workers[0].read_order + workers[1].read_order;
  int read_product = workers[0].read_order * workers[1].read_order;
  int write_sum = workers[0].write_order + workers[1].write_order;
  int write_product = workers[0].write_order * workers[1].write_order;

  if(workers[0].observed != 0 || workers[1].observed != 0 ||
     workers[0].written != 1 || workers[1].written != 1 ||
     snapshot->counter != 1 || read_sum != 3 || read_product != 2 ||
     write_sum != 7 || write_product != 12){
    printf("uthreadtest: racy mismatch observed=%d,%d written=%d,%d final=%d order=%d/%d,%d/%d\n",
           workers[0].observed, workers[1].observed,
           workers[0].written, workers[1].written, snapshot->counter,
           workers[0].read_order, workers[0].write_order,
           workers[1].read_order, workers[1].write_order);
    return -1;
  }
  return 0;
}

/**
 * 验证 LOCKED 模式把两个临界区串行化为 0→1 和 1→2。
 *
 * @param first_role 接收首先取得 counter_lock 的角色编号。
 * @return 顺序、计数和临界区边界全部成立返回 0，否则返回 -1。
 */
static int
validate_locked(struct concurrencylab_worker_snapshot *workers,
                struct concurrencylab_snapshot *snapshot, int *first_role)
{
  int first = workers[0].observed == 0 ? 0 : 1;
  int second = 1 - first;

  if(snapshot->counter != 2 ||
     workers[first].observed != 0 || workers[first].written != 1 ||
     workers[first].read_order != 1 || workers[first].write_order != 2 ||
     workers[second].observed != 1 || workers[second].written != 2 ||
     workers[second].read_order != 3 || workers[second].write_order != 4){
    printf("uthreadtest: locked mismatch first=%d observed=%d,%d written=%d,%d final=%d order=%d/%d,%d/%d\n",
           first, workers[0].observed, workers[1].observed,
           workers[0].written, workers[1].written, snapshot->counter,
           workers[0].read_order, workers[0].write_order,
           workers[1].read_order, workers[1].write_order);
    return -1;
  }
  *first_role = first;
  return 0;
}

/**
 * 执行一次完整会话，覆盖前置拒绝、两个参与者、快照、关闭和过期句柄。
 *
 * @param mode RACY 或 LOCKED。
 * @param round 从 1 开始的重复轮次，仅用于稳定输出。
 * @param session_out 接收本轮会话编号，供主函数验证生命周期单调前进。
 * @return 全部断言通过返回 0；任一创建、退出状态或行为断言失败返回 -1。
 */
static int
run_concurrency_round(int mode, int round, int *session_out)
{
  int session;
  int pids[CONCURRENCYLAB_PARTICIPANTS] = {-1, -1};
  int read_fds[CONCURRENCYLAB_PARTICIPANTS] = {-1, -1};
  int statuses[CONCURRENCYLAB_PARTICIPANTS] = {-1, -1};
  int active_reset;
  int premature_snapshot;
  int invalid_role;
  int stale_snapshot;
  int duplicate_close;
  int first_role = -1;
  struct concurrencylab_worker_snapshot workers[CONCURRENCYLAB_PARTICIPANTS];
  struct concurrencylab_worker_snapshot invalid_worker;
  struct concurrencylab_snapshot snapshot;

  session = concurrencylab(CONCURRENCYLAB_OP_RESET, mode, 0, 0);
  if(session <= 0){
    printf("uthreadtest: reset failed mode=%d session=%d\n", mode, session);
    return -1;
  }

  active_reset = concurrencylab(CONCURRENCYLAB_OP_RESET, mode, 0, 0);
  premature_snapshot = concurrencylab(CONCURRENCYLAB_OP_SNAPSHOT,
                                      session, 0, &snapshot);
  invalid_role = concurrencylab(CONCURRENCYLAB_OP_RUN,
                                session, CONCURRENCYLAB_PARTICIPANTS,
                                &invalid_worker);
  if(active_reset != -1 || premature_snapshot != -1 || invalid_role != -1){
    printf("uthreadtest: negative precondition active_reset=%d premature_snapshot=%d invalid_role=%d\n",
           active_reset, premature_snapshot, invalid_role);
    concurrencylab(CONCURRENCYLAB_OP_CLOSE, session, 0, 0);
    return -1;
  }

  if(start_worker(session, 0, &pids[0], &read_fds[0]) < 0){
    concurrencylab(CONCURRENCYLAB_OP_CLOSE, session, 0, 0);
    return -1;
  }
  if(start_worker(session, 1, &pids[1], &read_fds[1]) < 0){
    abort_workers(session, pids, read_fds, 1);
    return -1;
  }

  for(int role = 0; role < CONCURRENCYLAB_PARTICIPANTS; role++){
    if(read_full(read_fds[role], &workers[role], sizeof(workers[role])) < 0){
      abort_workers(session, pids, read_fds, CONCURRENCYLAB_PARTICIPANTS);
      return -1;
    }
    close(read_fds[role]);
    read_fds[role] = -1;
  }
  for(int role = 0; role < CONCURRENCYLAB_PARTICIPANTS; role++){
    int waited = waitpid(pids[role], &statuses[role], 0);
    if(waited != pids[role] || statuses[role] != 0){
      printf("uthreadtest: wait mismatch role=%d expected=%d actual=%d status=%d\n",
             role, pids[role], waited, statuses[role]);
      concurrencylab(CONCURRENCYLAB_OP_CLOSE, session, 0, 0);
      return -1;
    }
    pids[role] = -1;
  }

  if(concurrencylab(CONCURRENCYLAB_OP_SNAPSHOT, session, 0, &snapshot) < 0 ||
     validate_common(session, mode, workers, &snapshot) < 0){
    concurrencylab(CONCURRENCYLAB_OP_CLOSE, session, 0, 0);
    return -1;
  }
  if(mode == CONCURRENCYLAB_MODE_RACY){
    if(validate_racy(workers, &snapshot) < 0){
      concurrencylab(CONCURRENCYLAB_OP_CLOSE, session, 0, 0);
      return -1;
    }
  } else if(validate_locked(workers, &snapshot, &first_role) < 0){
    concurrencylab(CONCURRENCYLAB_OP_CLOSE, session, 0, 0);
    return -1;
  }

  if(concurrencylab(CONCURRENCYLAB_OP_CLOSE, session, 0, 0) < 0)
    return -1;
  stale_snapshot = concurrencylab(CONCURRENCYLAB_OP_SNAPSHOT,
                                  session, 0, &snapshot);
  duplicate_close = concurrencylab(CONCURRENCYLAB_OP_CLOSE, session, 0, 0);
  if(stale_snapshot != -1 || duplicate_close != -1){
    printf("uthreadtest: negative lifecycle stale_snapshot=%d duplicate_close=%d\n",
           stale_snapshot, duplicate_close);
    return -1;
  }

  if(mode == CONCURRENCYLAB_MODE_RACY){
    printf("CONCURRENCY RACY round=%d cpus=%d session=%d read_cpu=%d,%d write_cpu=%d,%d order=%d/%d,%d/%d final=1 lost_update=1\n",
           round, XV6_CPUS, session,
           workers[0].read_cpu, workers[1].read_cpu,
           workers[0].write_cpu, workers[1].write_cpu,
           workers[0].read_order, workers[0].write_order,
           workers[1].read_order, workers[1].write_order);
  } else {
    printf("CONCURRENCY LOCKED round=%d cpus=%d session=%d first_role=%d read_cpu=%d,%d write_cpu=%d,%d order=%d/%d,%d/%d final=2 atomic=1\n",
           round, XV6_CPUS, session, first_role,
           workers[0].read_cpu, workers[1].read_cpu,
           workers[0].write_cpu, workers[1].write_cpu,
           workers[0].read_order, workers[0].write_order,
           workers[1].read_order, workers[1].write_order);
  }
  printf("CONCURRENCY NEGATIVE round=%d active_reset=-1 premature_snapshot=-1 invalid_role=-1 stale_snapshot=-1 duplicate_close=-1\n",
         round);
  *session_out = session;
  return 0;
}

/**
 * 重复运行无锁和加锁会话，验证结果、负向 oracle 与资源复用。
 *
 * @return 四轮会话全部通过且 session 单调增长返回 0，否则返回 -1。
 */
static int
run_concurrency_regression(void)
{
  int previous_session = 0;
  int session = -1;

  if(concurrencylab(CONCURRENCYLAB_OP_RESET, 99, 0, 0) != -1){
    printf("uthreadtest: invalid mode accepted\n");
    return -1;
  }

  for(int round = 1; round <= CONCURRENCY_ROUNDS; round++){
    session = -1;
    if(run_concurrency_round(CONCURRENCYLAB_MODE_RACY, round, &session) < 0 ||
       session <= previous_session){
      printf("uthreadtest: racy FAILED round=%d session=%d previous=%d\n",
             round, session, previous_session);
      return -1;
    }
    previous_session = session;
  }
  for(int round = 1; round <= CONCURRENCY_ROUNDS; round++){
    session = -1;
    if(run_concurrency_round(CONCURRENCYLAB_MODE_LOCKED, round, &session) < 0 ||
       session <= previous_session){
      printf("uthreadtest: locked FAILED round=%d session=%d previous=%d\n",
             round, session, previous_session);
      return -1;
    }
    previous_session = session;
  }

  printf("uthreadtest: concurrency intro OK rounds=%d sessions=%d\n",
         CONCURRENCY_ROUNDS * 2, previous_session);
  return 0;
}

/** 运行用户线程调度与共享状态原子性两组并发回归。 */
int
main(void)
{
  if(run_uthread_regression() < 0 || run_concurrency_regression() < 0)
    exit(1);

  printf("uthreadtest: OK\n");
  exit(0);
}
