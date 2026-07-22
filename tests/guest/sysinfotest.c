#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/sysinfo.h"
#include "user/user.h"

#define TEST_PAGES 4

/**
 * 调用 sysinfo 并在系统调用失败时终止测试。
 *
 * @param info 接收内核统计信息的用户态缓冲区。
 */
static void
sinfo(struct sysinfo *info)
{
  if(sysinfo(info) < 0){
    printf("FAIL: sysinfo failed\n");
    exit(1);
  }
}

/**
 * 验证物理空闲页统计反映“首次触页”和“解除映射”，而不是仅反映 sbrk。
 *
 * 当前 xv6 使用 lazy allocation：sbrk() 只保留虚拟地址，真正的物理页在
 * 第一次写入时分配。因此测试固定触碰少量页面，并允许页表页等额外实现开销
 * 使空闲量下降超过 TEST_PAGES，但要求数据页本身至少被分配并最终归还。
 */
static void
testmem(void)
{
  struct sysinfo before;
  struct sysinfo touched;
  struct sysinfo released;
  uint64 bytes = TEST_PAGES * PGSIZE;

  sinfo(&before);

  char *base = sbrk(bytes);
  if(base == (char *)-1){
    printf("FAIL: sbrk reserve failed\n");
    exit(1);
  }

  // 首次写入触发 lazy allocation，确保统计的是实际物理页而非虚拟保留量。
  for(int page = 0; page < TEST_PAGES; page++)
    base[page * PGSIZE] = page;

  sinfo(&touched);
  if(before.freemem < touched.freemem + bytes){
    printf("FAIL: touching %d pages reduced free memory from %d to only %d\n",
           TEST_PAGES, before.freemem, touched.freemem);
    exit(1);
  }
  if((before.freemem - touched.freemem) % PGSIZE != 0){
    printf("FAIL: free memory changed by a non-page-aligned amount\n");
    exit(1);
  }

  if(sbrk(-bytes) == (char *)-1){
    printf("FAIL: sbrk release failed\n");
    exit(1);
  }

  sinfo(&released);
  if(released.freemem < touched.freemem + bytes){
    printf("FAIL: released pages did not return to allocator: touched=%d released=%d\n",
           touched.freemem, released.freemem);
    exit(1);
  }
}

/**
 * 验证 sysinfo 正常调用和非法用户指针回滚路径。
 */
static void
testcall(void)
{
  struct sysinfo info;

  if(sysinfo(&info) < 0){
    printf("FAIL: sysinfo failed\n");
    exit(1);
  }

  if(sysinfo((struct sysinfo *)0xeaeb0b5b00002f5e) != -1){
    printf("FAIL: sysinfo succeeded with bad argument\n");
    exit(1);
  }
}

/**
 * 验证 fork 创建和 wait 回收前后的活动进程计数。
 */
static void
testproc(void)
{
  struct sysinfo info;
  uint64 nproc;
  int status;
  int pid;

  sinfo(&info);
  nproc = info.nproc;

  pid = fork();
  if(pid < 0){
    printf("sysinfotest: fork failed\n");
    exit(1);
  }
  if(pid == 0){
    sinfo(&info);
    if(info.nproc != nproc + 1){
      printf("sysinfotest: FAIL nproc is %d instead of %d\n",
             info.nproc, nproc + 1);
      exit(1);
    }
    exit(0);
  }

  wait(&status);
  if(status != 0){
    printf("sysinfotest: child failed with status %d\n", status);
    exit(1);
  }

  sinfo(&info);
  if(info.nproc != nproc){
    printf("sysinfotest: FAIL nproc is %d instead of %d\n", info.nproc, nproc);
    exit(1);
  }
}

int
main(void)
{
  printf("sysinfotest: start\n");
  testcall();
  testmem();
  testproc();
  printf("sysinfotest: OK\n");
  exit(0);
}
