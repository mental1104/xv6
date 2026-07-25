#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "user/user.h"

/**
 * 输出当前测试阶段的诊断信息并以失败状态结束进程。
 *
 * @param phase 失败所属的行为阶段。
 * @param reason 可直接定位断言的失败原因。
 */
static void
fail(char *phase, char *reason)
{
  printf("memapitest: %s: %s\n", phase, reason);
  exit(1);
}

/**
 * 等待指定子进程并校验其退出状态。
 *
 * @param pid 需要回收的直接子进程 PID。
 * @param expected_status 期望由 wait() 观察到的退出状态。
 * @param phase 状态不符时用于诊断的测试阶段。
 */
static void
expect_child_status(int pid, int expected_status, char *phase)
{
  int status = 0;
  int waited_pid = wait(&status);

  if(waited_pid != pid)
    fail(phase, "wait returned an unexpected pid");
  if(status != expected_status)
    fail(phase, "child returned an unexpected status");
}

/**
 * 将当前 program break 提升到页边界，便于构造可重复的页内和跨页观察。
 *
 * @return 成功时返回补齐的字节数；sbrk 失败时返回 -1。
 */
static int
align_program_break(void)
{
  char *current = sbrk(0);
  uint64 address;
  int padding;

  if(current == (char *)-1)
    return -1;
  address = (uint64)current;
  padding = address % PGSIZE == 0 ? 0 : PGSIZE - address % PGSIZE;
  if(padding != 0 && sbrk(padding) == (char *)-1)
    return -1;
  return padding;
}

/**
 * 验证 malloc/free 由用户态分配器复用空闲块，而 free 不会直接收缩内核 break。
 */
static void
test_allocator_reuse(void)
{
  char *break_before = sbrk(0);
  char *first;
  char *break_after_first;
  char *second;

  if(break_before == (char *)-1)
    fail("allocator-reuse", "cannot read initial program break");

  first = malloc(1);
  if(first == 0)
    fail("allocator-reuse", "first malloc returned null");
  *(volatile char *)first = 0x5a;
  if(*(volatile char *)first != 0x5a)
    fail("allocator-reuse", "allocated byte did not round-trip");

  break_after_first = sbrk(0);
  if((uint64)break_after_first <= (uint64)break_before)
    fail("allocator-reuse", "first malloc did not request allocator arena");

  free(first);
  if(sbrk(0) != break_after_first)
    fail("allocator-reuse", "free unexpectedly changed program break");

  second = malloc(1);
  if(second == 0)
    fail("allocator-reuse", "second malloc returned null");
  if(sbrk(0) != break_after_first)
    fail("allocator-reuse", "second malloc failed to reuse free-list space");
  *(volatile char *)second = 0x33;
  if(*(volatile char *)second != 0x33)
    fail("allocator-reuse", "reused byte did not round-trip");
  free(second);
}

/**
 * 验证 fork 后父子拥有独立的分配器状态和写时复制视图，子进程释放与复用不会改写父进程对象。
 */
static void
test_fork_allocator_isolation(void)
{
  int *parent_value = malloc(sizeof(*parent_value));
  int pid;

  if(parent_value == 0)
    fail("fork-isolation", "parent malloc returned null");
  *parent_value = 41;

  pid = fork();
  if(pid < 0)
    fail("fork-isolation", "fork failed");
  if(pid == 0){
    int *child_value;

    free(parent_value);
    child_value = malloc(sizeof(*child_value));
    if(child_value == 0)
      exit(1);
    *child_value = 99;
    if(*child_value != 99)
      exit(1);
    free(child_value);
    exit(0);
  }

  expect_child_status(pid, 0, "fork-isolation");
  if(*parent_value != 41)
    fail("fork-isolation", "child allocator activity changed parent data");
  free(parent_value);
}

/**
 * 验证 sbrk 增长、触页、收缩和非法收缩请求的返回值与 program break 不变量。
 */
static void
test_sbrk_round_trip(void)
{
  char *before = sbrk(0);
  char *old_break;
  volatile char *region;

  if(before == (char *)-1)
    fail("sbrk-round-trip", "cannot read initial program break");

  old_break = sbrk(2 * PGSIZE);
  if(old_break != before)
    fail("sbrk-round-trip", "growth did not return the old break");
  if((uint64)sbrk(0) != (uint64)before + 2 * PGSIZE)
    fail("sbrk-round-trip", "growth did not advance the break");

  region = (volatile char *)before;
  region[0] = 0x11;
  region[2 * PGSIZE - 1] = 0x22;
  if(region[0] != 0x11 || region[2 * PGSIZE - 1] != 0x22)
    fail("sbrk-round-trip", "grown pages did not preserve writes");

  old_break = sbrk(-2 * PGSIZE);
  if((uint64)old_break != (uint64)before + 2 * PGSIZE)
    fail("sbrk-round-trip", "shrink did not return the old break");
  if(sbrk(0) != before)
    fail("sbrk-round-trip", "shrink did not restore the break");

  if(sbrk(-0x7fffffff) != (char *)-1)
    fail("sbrk-round-trip", "oversized shrink unexpectedly succeeded");
  if(sbrk(0) != before)
    fail("sbrk-round-trip", "failed shrink changed the break");
}

/**
 * 验证“越过逻辑边界必然立刻 fault”是错误直觉：同一已映射页内的一字节越界仍可能可见。
 *
 * 该观察仅用于说明页表保护粒度，不把越界访问升级为合法 API 用法；场景放在子进程中，
 * 由进程退出统一回收其临时地址空间。
 */
static void
test_page_granularity_counterexample(void)
{
  int pid = fork();

  if(pid < 0)
    fail("page-granularity", "fork failed");
  if(pid == 0){
    volatile char *one_byte;

    if(align_program_break() < 0)
      exit(1);
    one_byte = (volatile char *)sbrk(1);
    if(one_byte == (volatile char *)-1)
      exit(1);

    one_byte[0] = 0x44;
    one_byte[1] = 0x55;
    if(one_byte[0] != 0x44 || one_byte[1] != 0x55)
      exit(1);
    exit(0);
  }

  expect_child_status(pid, 0, "page-granularity");
}

/**
 * 验证收缩后的地址生命周期已经结束，首次再次访问会沿缺页路径以 exit(-1) 终止子进程。
 */
static void
test_shrink_invalidates_address(void)
{
  int pid = fork();

  if(pid < 0)
    fail("shrink-lifetime", "fork failed");
  if(pid == 0){
    volatile char *removed_page;

    if(align_program_break() < 0)
      exit(1);
    removed_page = (volatile char *)sbrk(PGSIZE);
    if(removed_page == (volatile char *)-1)
      exit(1);
    removed_page[0] = 0x66;
    if(sbrk(-PGSIZE) == (char *)-1)
      exit(1);

    // 正确结果是 usertrap 在本行访问时杀死子进程，不能到达后续 exit(2)。
    removed_page[0] = 0x77;
    exit(2);
  }

  expect_child_status(pid, -1, "shrink-lifetime");
}

/**
 * 依次执行内存 API 的用户分配器、地址空间和故障闭环。
 *
 * @param argc 命令行参数数量；本测试不接受额外参数。
 * @param argv 命令行参数数组。
 * @return 全部阶段通过时通过 exit(0) 结束；参数或断言失败时以非零状态结束。
 */
int
main(int argc, char *argv[])
{
  (void)argv;
  if(argc != 1)
    exit(2);

  test_allocator_reuse();
  test_fork_allocator_isolation();
  test_sbrk_round_trip();
  test_page_granularity_counterexample();
  test_shrink_invalidates_address();

  printf("memapitest: all checks passed\n");
  exit(0);
}
