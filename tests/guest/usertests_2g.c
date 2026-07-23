#include "kernel/memviz.h"

// 复用原始 usertests 的全部测试函数，但把依赖固定物理容量的入口重命名，
// 再在本文件提供适配 2 GiB 教学机配置的确定性版本。原始源码保持可直接
// 与上游对照，适配逻辑集中在单独的 C 翻译单元中。
#define sbrkbasic sbrkbasic_capacity_dependent
#define sbrkfail sbrkfail_capacity_dependent
#define run usertests_original_run
#define main usertests_original_main
#include "usertests.c"
#undef main
#undef run
#undef sbrkfail
#undef sbrkbasic

#define SBRK_ABORT_BYTES (64 * 1024 * 1024)
#define SBRK_ABORT_STRIDE (1024 * 1024)

static struct memviz_snapshot adapter_before;
static struct memviz_snapshot adapter_during;
static struct memviz_snapshot adapter_after;

/**
 * adapter_free_pages 返回当前可立即分配的物理页数。
 *
 * @return kalloc 所有 CPU freelist 的空闲页总数。
 *
 * 原 usertests 的 countfree() 会真正申请并触碰所有剩余物理页；RAM 提升到
 * 2 GiB 后，这会把一次资源泄漏检查放大成 GiB 级压力测试。memsnapshot
 * 读取同一个 allocator 状态，但不改变被观察对象。
 */
static uint64
adapter_free_pages(void)
{
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &adapter_before) < 0){
    printf("usertests: memsnapshot failed\n");
    exit(1);
  }
  return adapter_before.free_pages;
}

/**
 * reject_break_underflow 验证 sbrk 不允许把进程 break 缩到地址零以下。
 *
 * @param test_name 当前 usertests 名称，用于稳定错误输出。
 *
 * 该失败条件来自地址空间不变量，与 QEMU 提供 128 MiB 还是 2 GiB 无关，
 * 因而替代“申请固定 1 GiB 必须 OOM”的容量相关断言。
 */
static void
reject_break_underflow(char *test_name)
{
  char *current = sbrk(0);
  uint64 current_value = (uint64)current;
  if(current_value >= 0x7fffffffULL){
    printf("%s: initial break cannot fit underflow probe\n", test_name);
    exit(1);
  }

  int shrink = (int)current_value + 1;
  if(sbrk(-shrink) != (char*)0xffffffffffffffffL){
    printf("%s: sbrk accepted break below zero\n", test_name);
    exit(1);
  }
  if(sbrk(0) != current){
    printf("%s: failed sbrk changed break\n", test_name);
    exit(1);
  }
}

/**
 * sbrkbasic 保留原测试的小步增长、触页和 fork 语义，并使用确定性下溢
 * 错误替代固定 1 GiB OOM 假设。
 */
void
sbrkbasic(char *s)
{
  int i, pid, xstatus;
  char *c, *a, *b;

  reject_break_underflow(s);

  a = sbrk(0);
  for(i = 0; i < 5000; i++){
    b = sbrk(1);
    if(b != a){
      printf("%s: sbrk test failed %d %x %x\n", s, i, a, b);
      exit(1);
    }
    *b = 1;
    a = b + 1;
  }

  pid = fork();
  if(pid < 0){
    printf("%s: sbrk test fork failed\n", s);
    exit(1);
  }
  c = sbrk(1);
  c = sbrk(1);
  if(c != a + 1){
    printf("%s: sbrk test failed post-fork\n", s);
    exit(1);
  }
  if(pid == 0)
    exit(0);
  wait(&xstatus);
  exit(xstatus);
}

/**
 * sbrkfail 验证一个持有稀疏大地址空间的进程被终止后，已触碰页面、页表页
 * 和内核别名页表全部归还。
 *
 * 原测试通过触碰固定 1 GiB 来期待 OOM；在 2 GiB 机器上该请求可以合法
 * 成功，且测试成本随 RAM 容量增长。这里使用 64 MiB 虚拟范围、每 1 MiB
 * 触碰一页，再由父进程 kill/wait，保留错误中止与资源回收不变量。
 */
void
sbrkfail(char *s)
{
  int fds[2];
  int pid;
  int xstatus = 0;
  char ready;

  reject_break_underflow(s);
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &adapter_before) < 0){
    printf("%s: baseline memsnapshot failed\n", s);
    exit(1);
  }

  if(pipe(fds) != 0){
    printf("%s: pipe failed\n", s);
    exit(1);
  }

  pid = fork();
  if(pid < 0){
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if(pid == 0){
    close(fds[0]);
    char *base = sbrk(SBRK_ABORT_BYTES);
    if(base == (char*)0xffffffffffffffffL)
      exit(2);
    for(uint64 offset = 0; offset < SBRK_ABORT_BYTES; offset += SBRK_ABORT_STRIDE)
      base[offset] = (char)(offset / SBRK_ABORT_STRIDE);
    if(write(fds[1], "x", 1) != 1)
      exit(3);
    for(;;)
      sleep(1000);
  }

  close(fds[1]);
  if(read(fds[0], &ready, 1) != 1){
    printf("%s: child did not reach sparse allocation checkpoint\n", s);
    kill(pid);
    wait(0);
    close(fds[0]);
    exit(1);
  }
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &adapter_during) < 0){
    printf("%s: allocated memsnapshot failed\n", s);
    kill(pid);
    wait(0);
    close(fds[0]);
    exit(1);
  }
  if(adapter_during.free_pages >= adapter_before.free_pages){
    printf("%s: sparse worker did not consume physical pages\n", s);
    kill(pid);
    wait(0);
    close(fds[0]);
    exit(1);
  }

  kill(pid);
  wait(&xstatus);
  close(fds[0]);

  if(memsnapshot(MEMVIZ_VIEW_PHYS, &adapter_after) < 0){
    printf("%s: released memsnapshot failed\n", s);
    exit(1);
  }
  if(adapter_after.free_pages != adapter_before.free_pages){
    printf("%s: aborted sparse process leaked pages\n", s);
    exit(1);
  }
}

/**
 * run 在独立子进程中执行一个测试并根据退出状态报告结果。
 */
int
run(void f(char *), char *s)
{
  int pid;
  int xstatus;

  printf("test %s: ", s);
  if((pid = fork()) < 0){
    printf("runtest: fork error\n");
    exit(1);
  }
  if(pid == 0){
    f(s);
    exit(0);
  }

  wait(&xstatus);
  if(xstatus != 0)
    printf("FAILED\n");
  else
    printf("OK\n");
  return xstatus == 0;
}

/**
 * main 使用非破坏式物理页快照执行原 usertests 注册表。
 */
int
main(int argc, char *argv[])
{
  int continuous = 0;
  char *justone = 0;

  if(argc == 2 && strcmp(argv[1], "-c") == 0){
    continuous = 1;
  } else if(argc == 2 && strcmp(argv[1], "-C") == 0){
    continuous = 2;
  } else if(argc == 2 && argv[1][0] != '-'){
    justone = argv[1];
  } else if(argc > 1){
    printf("Usage: usertests [-c] [testname]\n");
    exit(1);
  }

  struct test {
    void (*f)(char *);
    char *s;
  } tests[] = {
    {execout, "execout"},
    {copyin, "copyin"},
    {copyout, "copyout"},
    {copyinstr1, "copyinstr1"},
    {copyinstr2, "copyinstr2"},
    {copyinstr3, "copyinstr3"},
    {truncate1, "truncate1"},
    {truncate2, "truncate2"},
    {truncate3, "truncate3"},
    {reparent2, "reparent2"},
    {jobctl, "jobctl"},
    {pgbug, "pgbug"},
    {sbrkbugs, "sbrkbugs"},
    {badarg, "badarg"},
    {reparent, "reparent"},
    {twochildren, "twochildren"},
    {forkfork, "forkfork"},
    {forkforkfork, "forkforkfork"},
    {argptest, "argptest"},
    {createdelete, "createdelete"},
    {linkunlink, "linkunlink"},
    {linktest, "linktest"},
    {unlinkread, "unlinkread"},
    {concreate, "concreate"},
    {subdir, "subdir"},
    {fourfiles, "fourfiles"},
    {sharedfd, "sharedfd"},
    {exectest, "exectest"},
    {bigargtest, "bigargtest"},
    {bigwrite, "bigwrite"},
    {bsstest, "bsstest"},
    {sbrkbasic, "sbrkbasic"},
    {sbrkmuch, "sbrkmuch"},
    {kernmem, "kernmem"},
    {sbrkfail, "sbrkfail"},
    {sbrkarg, "sbrkarg"},
    {validatetest, "validatetest"},
    {stacktest, "stacktest"},
    {opentest, "opentest"},
    {writetest, "writetest"},
    {writebig, "writebig"},
    {createtest, "createtest"},
    {openiputtest, "openiput"},
    {exitiputtest, "exitiput"},
    {iputtest, "iput"},
    {mem, "mem"},
    {pipe1, "pipe1"},
    {preempt, "preempt"},
    {exitwait, "exitwait"},
    {rmdot, "rmdot"},
    {fourteen, "fourteen"},
    {bigfile, "bigfile"},
    {dirfile, "dirfile"},
    {iref, "iref"},
    {forktest, "forktest"},
    {bigdir, "bigdir"},
    {0, 0},
  };

  if(continuous){
    printf("continuous usertests starting\n");
    while(1){
      int fail = 0;
      uint64 free0 = adapter_free_pages();
      for(struct test *t = tests; t->s != 0; t++){
        if(!run(t->f, t->s)){
          fail = 1;
          break;
        }
      }
      if(fail){
        printf("SOME TESTS FAILED\n");
        if(continuous != 2)
          exit(1);
      }
      uint64 free1 = adapter_free_pages();
      if(free1 < free0){
        printf("FAILED -- lost %d free pages\n", (int)(free0 - free1));
        if(continuous != 2)
          exit(1);
      }
    }
  }

  printf("usertests starting\n");
  uint64 free0 = adapter_free_pages();
  int fail = 0;
  for(struct test *t = tests; t->s != 0; t++){
    if((justone == 0) || strcmp(t->s, justone) == 0){
      if(!run(t->f, t->s))
        fail = 1;
    }
  }

  if(fail){
    printf("SOME TESTS FAILED\n");
    exit(1);
  }

  uint64 free1 = adapter_free_pages();
  if(free1 < free0){
    printf("FAILED -- lost some free pages %d (out of %d)\n",
           (int)free1, (int)free0);
    exit(1);
  }

  printf("ALL TESTS PASSED\n");
  exit(0);
}
