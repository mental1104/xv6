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
 * 在当前 PLIC 用户地址上限内保留一段足够大的稀疏测试区。
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
  if(address >= PLIC)
    return -1;

  uint64 room = PLIC - address;
  uint64 region = MAX_SPARSE_REGION;
  // sys_sbrk 要求 PGROUNDUP(newsz) 严格小于 PLIC，因此至少保留一页余量。
  if(region + PGSIZE >= room)
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
    printf("unable to reserve sparse region below PLIC\n");
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
    printf("unable to reserve sparse unmap region below PLIC\n");
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
