#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memviz.h"
#include "kernel/sysinfo.h"
#include "user/user.h"
#include "user/paths.h"

/**
 * 将上游 usertests 中固定的裸程序名映射为当前镜像绝对路径。
 *
 * @param path usertests 传给 exec() 的路径。
 * @param argv 子进程参数数组。
 * @return exec() 的返回值；成功时不返回，失败时返回 -1。
 *
 * 仅 `echo` 和 `sh` 是旧测试写死的镜像程序名。动态构造的相对路径、非法指针
 * 和故意不存在的文件均原样交给系统调用，避免改变 exec 错误路径覆盖。
 */
int
usertests_exec_absolute(char *path, char **argv)
{
  if(strcmp(path, "echo") == 0)
    return exec(XV6_BIN_PATH("echo"), argv);
  if(strcmp(path, "sh") == 0)
    return exec(XV6_BIN_PATH("sh"), argv);
  return exec(path, argv);
}

// 复用原始 usertests 的全部测试函数，但把依赖固定物理容量、旧文件语义或裸
// Shell 脚本的入口重命名，再在本文件提供适配当前教学内核的确定性版本。
#define exec usertests_exec_absolute
#define execout execout_capacity_dependent
#define jobctl jobctl_relative_commands
#define mem mem_capacity_dependent
#define sbrkbasic sbrkbasic_capacity_dependent
#define sbrkfail sbrkfail_capacity_dependent
#define truncate2 truncate2_pre_sparse_semantics
#define run usertests_original_run
#define main usertests_original_main
#include "usertests.c"
#undef main
#undef run
#undef truncate2
#undef sbrkfail
#undef sbrkbasic
#undef mem
#undef jobctl
#undef execout
#undef exec

#define EXECOUT_WORKING_SET (8 * 1024 * 1024)
#define MEM_ALLOCATIONS 2048
#define SBRK_ABORT_BYTES (64 * 1024 * 1024)
#define SBRK_ABORT_STRIDE (1024 * 1024)
#define DESCENDANT_REAP_WAIT_TICKS 100

static struct memviz_snapshot adapter_before;
static struct memviz_snapshot adapter_during;
static struct memviz_snapshot adapter_after;

/**
 * jobctl 验证 waitpid 兼容性以及非交互子 Shell 的后台作业生命周期。
 *
 * @param s usertests 统一测试名称，本函数不读取其内容。
 *
 * 子 Shell 没有 PATH；写入 pipe 的每条外部命令必须携带完整路径。jobs、fg 仍是
 * Shell 内置命令。输出断言也保留绝对命令文本，避免测试悄悄依赖根目录兼容链接。
 */
void
jobctl(char *s)
{
  char output[1024];
  char *argv[] = {XV6_BIN_PATH("sh"), 0};
  char *commands =
    "/bin/sleep 1 &\n"
    "/bin/sleep 3\n"
    "/bin/sleep 20 &\n"
    "/bin/echo foreground\n"
    "jobs\n"
    "fg %2\n"
    "/bin/echo fg-done\n";
  int command_len = strlen(commands);
  int input[2], result[2], pid, n, total = 0, status;

  (void)s;
  pid = fork();
  if(pid < 0)
    jobctlfail("waitpid fork failed");
  if(pid == 0){
    sleep(10);
    exit(7);
  }
  if(waitpid(pid, &status, WNOHANG) != 0)
    jobctlfail("WNOHANG did not return 0 for a running child");
  if(waitpid(pid + 100000, &status, WNOHANG) != -1)
    jobctlfail("waitpid accepted a non-child pid");
  if(waitpid(pid, &status, 2) != -1)
    jobctlfail("waitpid accepted unsupported options");
  if(waitpid(pid, &status, 0) != pid || status != 7)
    jobctlfail("blocking waitpid returned the wrong result");

  pid = fork();
  if(pid < 0)
    jobctlfail("compatibility fork failed");
  if(pid == 0)
    exit(9);
  if(wait(&status) != pid || status != 9)
    jobctlfail("wait compatibility regressed");

  if(pipe(input) < 0 || pipe(result) < 0)
    jobctlfail("pipe failed");
  pid = fork();
  if(pid < 0)
    jobctlfail("shell fork failed");
  if(pid == 0){
    close(input[1]);
    close(result[0]);
    close(0);
    dup(input[0]);
    close(1);
    dup(result[1]);
    close(2);
    dup(result[1]);
    close(input[0]);
    close(result[1]);
    exec(XV6_BIN_PATH("sh"), argv);
    exit(1);
  }

  close(input[0]);
  close(result[1]);
  if(write(input[1], commands, command_len) != command_len)
    jobctlfail("could not feed shell commands");
  close(input[1]);
  while(total + 1 < (int)sizeof(output) &&
        (n = read(result[0], output + total, sizeof(output) - total - 1)) > 0)
    total += n;
  output[total] = 0;
  close(result[0]);

  if(wait(&status) != pid || status != 0)
    jobctlfail("shell exited with an error");
  if(!jobctlcontains(output, "Done /bin/sleep 1 &") ||
     !jobctlcontains(output, "Running /bin/sleep 20 &") ||
     !jobctlcontains(output, "foreground") ||
     !jobctlcontains(output, "fg-done"))
    jobctlfail("shell job lifecycle output was incomplete");
}

/**
 * adapter_prime_snapshots 在采集资源基线前私有化快照缓冲区。
 *
 * usertests 的 run() 会先 fork 再执行具体测试，因此这些全局缓冲区最初与
 * 测试调度进程共享 COW 页面。memsnapshot() 先统计空闲页、再 copyout 整个
 * 快照；若第一次 copyout 才触发 COW，返回的计数不会包含这次合法分配，
 * 后续比较就会把测试进程自身的私有页误判为被测 worker 泄漏。
 *
 * 这里在建立基线前主动写满三个输出缓冲区，使 COW 成本被基线包含。之后
 * worker 退出时，资源比较只反映被测地址空间、页表和内核别名是否被回收。
 */
static void
adapter_prime_snapshots(void)
{
  memset(&adapter_before, 0, sizeof(adapter_before));
  memset(&adapter_during, 0, sizeof(adapter_during));
  memset(&adapter_after, 0, sizeof(adapter_after));
}

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
 * adapter_process_count 返回当前尚未进入 UNUSED 的进程数量。
 *
 * @return init、Shell、测试调度进程及其全部活跃后代的总数。
 */
static uint64
adapter_process_count(void)
{
  struct sysinfo info;

  if(sysinfo(&info) < 0){
    printf("usertests: sysinfo failed\n");
    exit(1);
  }
  return info.nproc;
}

/**
 * adapter_wait_for_process_baseline 等待由 init 异步回收的测试后代全部释放。
 *
 * @param test_name 当前测试名称，用于输出可定位的超时错误。
 * @param baseline 启动测试 worker 前的进程总数。
 * @return 进程数在有界时间内恢复到 baseline 或更低时返回 0，否则返回 -1。
 *
 * reparent 和 reparent2 会故意制造由 init 接管的孙进程。父测试进程 wait() 只
 * 保证直接 worker 已回收，不能证明这些后代已经经过 freeproc()。物理页快照
 * 必须在进程数收敛后采集，否则会把合法的异步回收窗口误判成内存泄漏。
 */
static int
adapter_wait_for_process_baseline(char *test_name, uint64 baseline)
{
  uint64 current = adapter_process_count();

  for(int waited = 0;
      current > baseline && waited < DESCENDANT_REAP_WAIT_TICKS;
      waited++){
    sleep(1);
    current = adapter_process_count();
  }
  if(current <= baseline)
    return 0;

  printf("\n%s: descendant cleanup timed out nproc=%d baseline=%d\n",
         test_name, (int)current, (int)baseline);
  return -1;
}

/**
 * execout 验证 exec 在构造新地址空间后失败时会回滚所有临时页面。
 *
 * @param s 当前 usertests 名称，用于稳定错误输出。
 *
 * 原测试通过反复写满全部物理内存制造 OOM；2 GiB 配置下会产生几十 GiB
 * 的无效内存写入。这里让子进程先触碰固定工作集，再以超过 MAXARG 的 argv
 * 迫使 exec 在新页表和用户栈构造阶段失败。若 exec 错误地成功，sleep 会因
 * 参数数量错误而返回非零；若按预期失败，子进程从 exec 返回并以 0 退出。
 */
void
execout(char *s)
{
  adapter_prime_snapshots();
  uint64 free0 = adapter_free_pages();

  for(int avail = 0; avail < 15; avail++){
    int pid = fork();
    if(pid < 0){
      printf("%s: execout fork failed\n", s);
      exit(1);
    }
    if(pid == 0){
      int bytes = EXECOUT_WORKING_SET + 15 * PGSIZE;
      char *base = sbrk(bytes);
      if(base == (char*)0xffffffffffffffffL)
        exit(2);
      for(int offset = 0; offset < bytes; offset += PGSIZE)
        base[offset] = (char)(offset / PGSIZE);
      if(avail > 0 && sbrk(-avail * PGSIZE) == (char*)0xffffffffffffffffL)
        exit(3);

      char *args[MAXARG + 2];
      for(int i = 0; i <= MAXARG; i++)
        args[i] = "x";
      args[MAXARG + 1] = 0;
      exec(XV6_BIN_PATH("sleep"), args);
      exit(0);
    }

    int status = -1;
    wait(&status);
    if(status != 0){
      printf("%s: bounded exec rollback failed status=%d\n", s, status);
      exit(1);
    }
    if(adapter_free_pages() != free0){
      printf("%s: failed exec leaked pages\n", s);
      exit(1);
    }
  }
}

/**
 * mem 验证 malloc 页面可写、链式释放和再次分配，不再依赖耗尽整机内存。
 *
 * @param s 当前 usertests 名称，用于稳定错误输出。
 */
void
mem(char *s)
{
  adapter_prime_snapshots();
  uint64 free0 = adapter_free_pages();
  int pid = fork();
  if(pid < 0){
    printf("%s: mem fork failed\n", s);
    exit(1);
  }
  if(pid == 0){
    void *head = 0;
    for(int i = 0; i < MEM_ALLOCATIONS; i++){
      void *block = malloc(10001);
      if(block == 0){
        printf("%s: bounded malloc failed at %d\n", s, i);
        exit(1);
      }
      *(void**)block = head;
      ((char*)block)[10000] = (char)i;
      head = block;
    }

    while(head){
      void *next = *(void**)head;
      free(head);
      head = next;
    }

    void *again = malloc(1024 * 20);
    if(again == 0){
      printf("%s: allocator did not reuse released pages\n", s);
      exit(1);
    }
    memset(again, 0x5a, 1024 * 20);
    free(again);
    exit(0);
  }

  int status = -1;
  wait(&status);
  if(status != 0)
    exit(status);
  if(adapter_free_pages() != free0){
    printf("%s: bounded malloc worker leaked pages\n", s);
    exit(1);
  }
}

/**
 * reject_break_underflow 验证 sbrk 不允许把进程 break 缩到地址零以下。
 *
 * @param test_name 当前测试名称，用于稳定错误输出。
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
  adapter_prime_snapshots();
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
 * truncate2 验证截断不会回退其他打开文件对象的偏移，并且后续越过 EOF 的
 * 写入会建立可持久化的稀疏文件，而不是沿用上游 xv6 的失败语义。
 *
 * @param s 当前 usertests 名称，用于稳定错误输出。
 *
 * fd1 写入四字节后停在偏移 4；fd2 将同一 inode 截断到零，但不会修改 fd1
 * 持有的独立 file offset。fd1 随后写入一个字节，逻辑文件应扩展到五字节，
 * 偏移 0..3 成为读零的 hole，偏移 4 保存写入的 x。
 */
void
truncate2(char *s)
{
  char data[5];
  struct stat st;

  unlink("truncfile");

  int fd1 = open("truncfile", O_CREATE|O_TRUNC|O_WRONLY);
  if(fd1 < 0){
    printf("%s: create failed\n", s);
    exit(1);
  }
  if(write(fd1, "abcd", 4) != 4){
    printf("%s: initial write failed\n", s);
    close(fd1);
    unlink("truncfile");
    exit(1);
  }

  int fd2 = open("truncfile", O_TRUNC|O_WRONLY);
  if(fd2 < 0){
    printf("%s: truncate open failed\n", s);
    close(fd1);
    unlink("truncfile");
    exit(1);
  }
  close(fd2);

  if(write(fd1, "x", 1) != 1){
    printf("%s: sparse write after truncate failed\n", s);
    close(fd1);
    unlink("truncfile");
    exit(1);
  }
  close(fd1);

  // 重开文件验证 hole 与尾部数据已经进入 inode，而非只存在于旧 file offset。
  int fd = open("truncfile", O_RDONLY);
  if(fd < 0){
    printf("%s: reopen failed\n", s);
    unlink("truncfile");
    exit(1);
  }
  if(fstat(fd, &st) < 0 || st.size != sizeof(data)){
    printf("%s: sparse size %d, expected %d\n",
           s, (int)st.size, (int)sizeof(data));
    close(fd);
    unlink("truncfile");
    exit(1);
  }
  memset(data, 0x7f, sizeof(data));
  if(read(fd, data, sizeof(data)) != sizeof(data)){
    printf("%s: sparse read failed\n", s);
    close(fd);
    unlink("truncfile");
    exit(1);
  }
  for(int i = 0; i < 4; i++){
    if(data[i] != 0){
      printf("%s: hole byte %d is %d, expected 0\n", s, i, data[i]);
      close(fd);
      unlink("truncfile");
      exit(1);
    }
  }
  if(data[4] != 'x'){
    printf("%s: sparse tail is %d, expected x\n", s, data[4]);
    close(fd);
    unlink("truncfile");
    exit(1);
  }

  close(fd);
  if(unlink("truncfile") < 0){
    printf("%s: unlink failed\n", s);
    exit(1);
  }
}

/**
 * run 在独立子进程中执行一个测试，并等待其交给 init 的后代完成回收。
 *
 * @param f 被执行的测试函数。
 * @param s 当前测试名称，用于输出结果和回收超时诊断。
 * @return 测试成功且后代进程数恢复到启动前基线时返回 1，否则返回 0。
 */
int
run(void f(char *), char *s)
{
  int pid;
  int xstatus;
  uint64 process_baseline = adapter_process_count();

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
  int cleanup_status = adapter_wait_for_process_baseline(s, process_baseline);
  if(xstatus != 0 || cleanup_status < 0)
    printf("FAILED\n");
  else
    printf("OK\n");
  return xstatus == 0 && cleanup_status == 0;
}

/** main 使用非破坏式物理页快照执行原 usertests 注册表。 */
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
