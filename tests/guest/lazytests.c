#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"

#define MAX_SPARSE_REGION (64 * 1024 * 1024)
#define OOM_CHUNK_SIZE (4 * 1024 * 1024)

/**
 * 在当前 USERMAX 用户地址上限内保留一段足够大的稀疏测试区。
 *
 * @param start 接收 sbrk 前的旧 break。
 * @return 成功时返回页对齐区域大小；可用虚拟空间不足或 sbrk 失败时返回 -1。
 */
static int
reserve_sparse_region(char **start)
{
  char *current = sbrk(0);
  if(current == (char *)-1)
    return -1;

  uint64 address = (uint64)current;
  if(address >= USERMAX)
    return -1;

  uint64 room = USERMAX - address;
  uint64 region = MAX_SPARSE_REGION;
  // 为后续边界测试保留一页余量；sys_sbrk 允许 break 恰好到达 USERMAX。
  if(region + PGSIZE > room)
    region = PGROUNDDOWN(room / 2);
  if(region < 4 * PGSIZE || region > 0x7fffffff)
    return -1;

  char *previous = sbrk((int)region);
  if(previous == (char *)-1)
    return -1;

  *start = previous;
  return (int)region;
}

void
sparse_memory(char *s)
{
  char *i, *prev_end, *new_end;
  int region_size = reserve_sparse_region(&prev_end);

  if(region_size < 0){
    printf("unable to reserve sparse region below USERMAX\n");
    exit(1);
  }
  new_end = prev_end + region_size;

  // 每 64 页只触碰一页，验证 sbrk 保留的其余虚拟页不会立即物化。
  for(i = prev_end + PGSIZE; i < new_end; i += 64 * PGSIZE)
    *(char **)i = i;

  for(i = prev_end + PGSIZE; i < new_end; i += 64 * PGSIZE){
    if(*(char **)i != i){
      printf("failed to read value from sparse memory\n");
      exit(1);
    }
  }

  exit(0);
}

void
sparse_memory_unmap(char *s)
{
  int pid;
  char *i, *prev_end, *new_end;
  int region_size = reserve_sparse_region(&prev_end);

  if(region_size < 0){
    printf("unable to reserve sparse unmap region below USERMAX\n");
    exit(1);
  }
  new_end = prev_end + region_size;

  for(i = prev_end + PGSIZE; i < new_end; i += PGSIZE * PGSIZE)
    *(char **)i = i;

  for(i = prev_end + PGSIZE; i < new_end; i += PGSIZE * PGSIZE){
    pid = fork();
    if(pid < 0){
      printf("error forking\n");
      exit(1);
    } else if(pid == 0){
      if(sbrk(-region_size) == (char *)-1)
        exit(1);
      // 该地址已被 shrink 移出 p->sz，正确行为是 usertrap 以 -1 杀死子进程。
      *(char **)i = i;
      exit(0);
    } else {
      int status;
      wait(&status);
      if(status != -1){
        printf("memory not unmapped, child status=%d\n", status);
        exit(1);
      }
    }
  }

  exit(0);
}

void
oom(char *s)
{
  int pid = fork();
  if(pid < 0){
    printf("oom: fork failed\n");
    exit(1);
  }

  if(pid == 0){
    for(;;){
      char *base = sbrk(OOM_CHUNK_SIZE);
      if(base == (char *)-1){
        // 先到虚拟地址上限说明没有真正触发物理 OOM，父进程必须判失败。
        exit(0);
      }
      // volatile 写遍每一页，确保 lazy allocation 真正申请物理页。
      for(int offset = 0; offset < OOM_CHUNK_SIZE; offset += PGSIZE)
        *(volatile char *)(base + offset) = 1;
    }
  }

  int status;
  wait(&status);
  // 当前 page-fault OOM 路径通过 exit(-1) 终止子进程。
  exit(status == -1 ? 0 : 1);
}

/**
 * 输出内存 API 测试阶段和原因，并结束当前测试进程。
 *
 * @param phase 失败所属的行为阶段。
 * @param reason 可直接定位断言的失败原因。
 */
static void
memory_api_fail(char *phase, char *reason)
{
  printf("memory-api: %s: %s\n", phase, reason);
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
memory_api_expect_child(int pid, int expected_status, char *phase)
{
  int status = 0;
  int waited_pid = wait(&status);

  if(waited_pid != pid)
    memory_api_fail(phase, "wait returned an unexpected pid");
  if(status != expected_status)
    memory_api_fail(phase, "child returned an unexpected status");
}

/**
 * 将当前 program break 提升到页边界，便于构造可重复的页内和跨页观察。
 *
 * @return 成功时返回补齐的字节数；sbrk 失败时返回 -1。
 */
static int
memory_api_align_break(void)
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
memory_api_allocator_reuse(void)
{
  char *break_before = sbrk(0);
  char *first;
  char *break_after_first;
  char *second;

  if(break_before == (char *)-1)
    memory_api_fail("allocator-reuse", "cannot read initial program break");

  first = malloc(1);
  if(first == 0)
    memory_api_fail("allocator-reuse", "first malloc returned null");
  *(volatile char *)first = 0x5a;
  if(*(volatile char *)first != 0x5a)
    memory_api_fail("allocator-reuse", "allocated byte did not round-trip");

  break_after_first = sbrk(0);
  if((uint64)break_after_first <= (uint64)break_before)
    memory_api_fail("allocator-reuse", "first malloc did not request allocator arena");

  free(first);
  if(sbrk(0) != break_after_first)
    memory_api_fail("allocator-reuse", "free unexpectedly changed program break");

  second = malloc(1);
  if(second == 0)
    memory_api_fail("allocator-reuse", "second malloc returned null");
  if(sbrk(0) != break_after_first)
    memory_api_fail("allocator-reuse", "second malloc failed to reuse free-list space");
  *(volatile char *)second = 0x33;
  if(*(volatile char *)second != 0x33)
    memory_api_fail("allocator-reuse", "reused byte did not round-trip");
  free(second);
}

/**
 * 验证 fork 后父子拥有独立的分配器状态和写时复制视图，子进程释放与复用不会改写父进程对象。
 */
static void
memory_api_fork_isolation(void)
{
  int *parent_value = malloc(sizeof(*parent_value));
  int pid;

  if(parent_value == 0)
    memory_api_fail("fork-isolation", "parent malloc returned null");
  *parent_value = 41;

  pid = fork();
  if(pid < 0)
    memory_api_fail("fork-isolation", "fork failed");
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

  memory_api_expect_child(pid, 0, "fork-isolation");
  if(*parent_value != 41)
    memory_api_fail("fork-isolation", "child allocator activity changed parent data");
  free(parent_value);
}

/**
 * 验证 sbrk 增长、触页、收缩和非法收缩请求的返回值与 program break 不变量。
 */
static void
memory_api_sbrk_round_trip(void)
{
  char *before = sbrk(0);
  char *old_break;
  volatile char *region;

  if(before == (char *)-1)
    memory_api_fail("sbrk-round-trip", "cannot read initial program break");

  old_break = sbrk(2 * PGSIZE);
  if(old_break != before)
    memory_api_fail("sbrk-round-trip", "growth did not return the old break");
  if((uint64)sbrk(0) != (uint64)before + 2 * PGSIZE)
    memory_api_fail("sbrk-round-trip", "growth did not advance the break");

  region = (volatile char *)before;
  region[0] = 0x11;
  region[2 * PGSIZE - 1] = 0x22;
  if(region[0] != 0x11 || region[2 * PGSIZE - 1] != 0x22)
    memory_api_fail("sbrk-round-trip", "grown pages did not preserve writes");

  old_break = sbrk(-2 * PGSIZE);
  if((uint64)old_break != (uint64)before + 2 * PGSIZE)
    memory_api_fail("sbrk-round-trip", "shrink did not return the old break");
  if(sbrk(0) != before)
    memory_api_fail("sbrk-round-trip", "shrink did not restore the break");

  if(sbrk(-0x7fffffff) != (char *)-1)
    memory_api_fail("sbrk-round-trip", "oversized shrink unexpectedly succeeded");
  if(sbrk(0) != before)
    memory_api_fail("sbrk-round-trip", "failed shrink changed the break");
}

/**
 * 验证“越过逻辑边界必然立刻 fault”是错误直觉：同一已映射页内的一字节越界仍可能可见。
 *
 * 该观察仅用于说明页表保护粒度，不把越界访问升级为合法 API 用法；场景放在子进程中，
 * 由进程退出统一回收其临时地址空间。
 */
static void
memory_api_page_granularity(void)
{
  int pid = fork();

  if(pid < 0)
    memory_api_fail("page-granularity", "fork failed");
  if(pid == 0){
    volatile char *one_byte;

    if(memory_api_align_break() < 0)
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

  memory_api_expect_child(pid, 0, "page-granularity");
}

/**
 * 验证收缩后的地址生命周期已经结束，首次再次访问会沿缺页路径以 exit(-1) 终止子进程。
 */
static void
memory_api_shrink_lifetime(void)
{
  int pid = fork();

  if(pid < 0)
    memory_api_fail("shrink-lifetime", "fork failed");
  if(pid == 0){
    volatile char *removed_page;

    if(memory_api_align_break() < 0)
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

  memory_api_expect_child(pid, -1, "shrink-lifetime");
}

/**
 * 闭环验证用户态分配器、sbrk 地址空间边界、fork 隔离和失效地址故障路径。
 *
 * @param s 测试注册名称，仅用于兼容 lazytests 的统一函数签名。
 */
void
memory_api(char *s)
{
  (void)s;
  memory_api_allocator_reuse();
  memory_api_fork_isolation();
  memory_api_sbrk_round_trip();
  memory_api_page_granularity();
  memory_api_shrink_lifetime();
  printf("memory-api: all checks passed\n");
  exit(0);
}

// run each test in its own process. run returns 1 if child's exit()
// indicates success.
int
run(void f(char *), char *s)
{
  int pid;
  int xstatus;

  printf("running test %s\n", s);
  if((pid = fork()) < 0){
    printf("runtest: fork error\n");
    exit(1);
  }
  if(pid == 0){
    f(s);
    exit(0);
  } else {
    wait(&xstatus);
    if(xstatus != 0)
      printf("test %s: FAILED\n", s);
    else
      printf("test %s: OK\n", s);
    return xstatus == 0;
  }
}

/**
 * 执行 lazy allocation 测试并通过进程退出状态返回汇总结果。
 *
 * @param argc 命令行参数数量；可选第二个参数用于只运行同名子测试。
 * @param argv 参数数组；argv[1] 存在时必须与测试名称精确匹配。
 * @return 全部选中测试通过时通过 exit(0) 结束，任一失败时通过 exit(1) 结束。
 */
int
main(int argc, char *argv[])
{
  char *n = 0;
  if(argc > 1)
    n = argv[1];

  struct test {
    void (*f)(char *);
    char *s;
  } tests[] = {
    {sparse_memory, "lazy alloc"},
    {sparse_memory_unmap, "lazy unmap"},
    {oom, "out of memory"},
    {memory_api, "memory-api"},
    {0, 0},
  };

  printf("lazytests starting\n");

  int fail = 0;
  for(struct test *t = tests; t->s != 0; t++){
    if((n == 0) || strcmp(t->s, n) == 0){
      if(!run(t->f, t->s))
        fail = 1;
    }
  }
  if(!fail)
    printf("ALL TESTS PASSED\n");
  else
    printf("SOME TESTS FAILED\n");

  exit(fail);
}
