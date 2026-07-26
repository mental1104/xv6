#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/locklab.h"
#include "user/user.h"

#define SPLIT_WORKERS 2
#define SAFE_WORKERS 4
#define SAFE_INCREMENTS 200
#define STATE_WAIT_TICKS 200

/**
 * 输出稳定失败原因并终止当前测试进程。
 *
 * @param reason 不含 runner 拒绝关键字的简短诊断文本。
 */
static void
fail(char *reason)
{
  printf("locktest: %s\n", reason);
  exit(1);
}

/**
 * 从管道读取一个同步字节。
 *
 * @param fd 可读管道描述符。
 * @return 成功读取一个字节返回 0；EOF 或错误返回 -1。
 */
static int
read_token(int fd)
{
  char token;
  return read(fd, &token, 1) == 1 ? 0 : -1;
}

/**
 * 向管道写入一个同步字节。
 *
 * @param fd 可写管道描述符。
 * @return 成功写入一个字节返回 0；短写或错误返回 -1。
 */
static int
write_token(int fd)
{
  char token = 'x';
  return write(fd, &token, 1) == 1 ? 0 : -1;
}

/**
 * 回收指定数量的直接子进程并要求全部正常退出。
 *
 * @param count 期望回收的子进程数量。
 * @return 全部退出状态为 0 时返回 0；wait 或任一子进程失败时返回 -1。
 */
static int
wait_successful_children(int count)
{
  for(int i = 0; i < count; i++){
    int status = -1;
    if(wait(&status) < 0 || status != 0)
      return -1;
  }
  return 0;
}

/**
 * 等待睡眠实验状态满足指定位图条件。
 *
 * @param mask 参与比较的 LOCKLAB_SLEEP_* 位。
 * @param expected mask 范围内的期望值。
 * @return 在超时前满足条件时返回完整状态；超时或系统调用失败时返回 -1。
 */
static int
wait_for_state(int mask, int expected)
{
  for(int tick = 0; tick < STATE_WAIT_TICKS; tick++){
    int state = locklab(LOCKLAB_SLEEP_STATE, 0);
    if(state < 0)
      return -1;
    if((state & mask) == expected)
      return state;
    sleep(1);
  }
  return -1;
}

/**
 * 稳定复现“每次访问都加锁，但完整读改写未形成同一临界区”的丢失更新。
 *
 * @return 两个进程都从 0 计算出 1，最终计数器稳定为 1 时返回 0。
 */
static int
check_split_critical_section(void)
{
  int ready[2];
  int go[2];

  if(locklab(LOCKLAB_RESET, 0) != 0)
    return -1;
  if(pipe(ready) < 0 || pipe(go) < 0)
    return -1;

  for(int worker = 0; worker < SPLIT_WORKERS; worker++){
    int pid = fork();
    if(pid < 0)
      return -1;
    if(pid == 0){
      close(ready[0]);
      close(go[1]);

      int observed = locklab(LOCKLAB_SPLIT_READ, 0);
      if(observed != 0 || write_token(ready[1]) < 0 || read_token(go[0]) < 0)
        exit(1);
      if(locklab(LOCKLAB_SPLIT_WRITE, observed + 1) != 0)
        exit(1);

      close(ready[1]);
      close(go[0]);
      exit(0);
    }
  }

  close(ready[1]);
  close(go[0]);
  for(int worker = 0; worker < SPLIT_WORKERS; worker++)
    if(read_token(ready[0]) < 0)
      return -1;
  for(int worker = 0; worker < SPLIT_WORKERS; worker++)
    if(write_token(go[1]) < 0)
      return -1;
  close(ready[0]);
  close(go[1]);

  if(wait_successful_children(SPLIT_WORKERS) < 0)
    return -1;
  return locklab(LOCKLAB_COUNTER, 0) == 1 ? 0 : -1;
}

/**
 * 验证完整读改写位于一个 spinlock 临界区时不会丢失更新。
 *
 * @return 多进程并发递增后的精确计数满足不变量时返回 0。
 */
static int
check_spinlock_counter(void)
{
  int go[2];

  if(locklab(LOCKLAB_RESET, 0) != 0 || pipe(go) < 0)
    return -1;

  for(int worker = 0; worker < SAFE_WORKERS; worker++){
    int pid = fork();
    if(pid < 0)
      return -1;
    if(pid == 0){
      close(go[1]);
      if(read_token(go[0]) < 0)
        exit(1);
      for(int step = 0; step < SAFE_INCREMENTS; step++)
        if(locklab(LOCKLAB_SAFE_INCREMENT, 0) < 1)
          exit(1);
      close(go[0]);
      exit(0);
    }
  }

  close(go[0]);
  for(int worker = 0; worker < SAFE_WORKERS; worker++)
    if(write_token(go[1]) < 0)
      return -1;
  close(go[1]);

  if(wait_successful_children(SAFE_WORKERS) < 0)
    return -1;
  return locklab(LOCKLAB_COUNTER, 0) ==
         SAFE_WORKERS * SAFE_INCREMENTS ? 0 : -1;
}

/**
 * 验证错误持有者检查以及 push_off/pop_off 的嵌套边界。
 *
 * @return 内核返回完整 LOCKLAB_OWNER_EXPECTED 位图时返回 0。
 */
static int
check_owner_oracle(void)
{
  return locklab(LOCKLAB_OWNER_ORACLE, 0) == LOCKLAB_OWNER_EXPECTED ? 0 : -1;
}

/**
 * 验证 sleeplock 持有者可以睡眠，等待者在释放前不能取得所有权。
 *
 * @return holder/waiter 状态按约束推进且两个子进程正常退出时返回 0。
 */
static int
check_sleeplock_wait(void)
{
  if(locklab(LOCKLAB_RESET, 0) != 0)
    return -1;

  int holder = fork();
  if(holder < 0)
    return -1;
  if(holder == 0)
    exit(locklab(LOCKLAB_SLEEP_HOLDER, 0) == 0 ? 0 : 1);

  if(wait_for_state(LOCKLAB_SLEEP_HOLDER_READY,
                    LOCKLAB_SLEEP_HOLDER_READY) < 0)
    return -1;

  int waiter = fork();
  if(waiter < 0)
    return -1;
  if(waiter == 0)
    exit(locklab(LOCKLAB_SLEEP_WAITER, 0) == 0 ? 0 : 1);

  if(wait_for_state(LOCKLAB_SLEEP_WAITER_STARTED,
                    LOCKLAB_SLEEP_WAITER_STARTED) < 0)
    return -1;

  // waiter 已经进入获取路径，但 holder 尚未收到释放条件；此时取得锁即为错误。
  for(int tick = 0; tick < 8; tick++){
    int state = locklab(LOCKLAB_SLEEP_STATE, 0);
    if(state < 0 || (state & LOCKLAB_SLEEP_WAITER_ACQUIRED) != 0)
      return -1;
    sleep(1);
  }

  if(locklab(LOCKLAB_SLEEP_RELEASE, 0) != 0)
    return -1;
  if(wait_successful_children(2) < 0)
    return -1;

  int state = locklab(LOCKLAB_SLEEP_STATE, 0);
  if(state < 0 || (state & LOCKLAB_SLEEP_WAITER_ACQUIRED) == 0)
    return -1;
  if((state & LOCKLAB_SLEEP_HOLDER_READY) != 0)
    return -1;
  return 0;
}

/**
 * 执行一轮完整锁模型回归。
 *
 * @return 所有正常、反例、睡眠和错误输入断言通过时返回 0。
 */
static int
run_round(void)
{
  if(check_split_critical_section() < 0)
    return -1;
  if(check_spinlock_counter() < 0)
    return -1;
  if(check_owner_oracle() < 0)
    return -1;
  if(check_sleeplock_wait() < 0)
    return -1;
  if(locklab(9999, 0) != -1)
    return -1;
  return 0;
}

/**
 * locktest 用户入口。
 *
 * @param argc 必须为 2。
 * @param argv argv[1] 必须为 positive。
 * @return 不直接返回；两轮可重复回归成功 exit(0)，否则 exit(non-zero)。
 */
int
main(int argc, char **argv)
{
  if(argc != 2 || strcmp(argv[1], "positive") != 0){
    fprintf(2, "Usage: locktest positive\n");
    exit(2);
  }

  for(int round = 0; round < 2; round++)
    if(run_round() < 0)
      fail("invariant mismatch");

  printf("locktest: ok\n");
  exit(0);
}
