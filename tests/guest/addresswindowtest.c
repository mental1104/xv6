#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"
#include "kernel/fcntl.h"
#include "kernel/memviz.h"
#include "user/user.h"
#include "user/paths.h"

static struct memviz_snapshot snapshot;
static struct memviz_va_query query;

/**
 * fail 输出稳定失败原因并终止当前测试进程。
 *
 * @param message 便于 CI 定位的短文本。
 */
static void
fail(char *message)
{
  printf("addresswindowtest: FAIL: %s\n", message);
  exit(1);
}

/**
 * free_pages 返回当前 kalloc 可立即分配页数。
 *
 * @return 所有 CPU freelist 中的空闲物理页总数。
 */
static uint64
free_pages(void)
{
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &snapshot) < 0)
    fail("memsnapshot failed");
  return snapshot.free_pages;
}

/**
 * require_present 断言用户页表已经为指定数值 VA 建立叶子映射。
 *
 * @param va 已被用户态 load/store 触碰的地址。
 */
static void
require_present(uint64 va)
{
  if(vaquery(va, &query) < 0 || !query.present)
    fail("expected user mapping missing");
}

/**
 * grow_to 使用多个 int 范围内的 sbrk 调用推进 p->sz。
 *
 * @param target 目标 break，必须小于等于 USERMAX。
 *
 * 单次 sbrk 仍保留 xv6 的 32 位 ABI；循环推进用于验证跨越 2 GiB direct-map
 * 数值时，用户页仍通过 Sv39 高半区 alias 被内核访问。
 */
static void
grow_to(uint64 target)
{
  uint64 current = (uint64)sbrk(0);
  if(target > USERMAX || current > target)
    fail("invalid grow target");

  while(current < target){
    uint64 remaining = target - current;
    int step = remaining > 0x7ffff000ULL ? 0x7ffff000 : (int)remaining;
    if((uint64)sbrk(step) != current)
      fail("sbrk chunk failed");
    current += (uint64)step;
  }
}

/**
 * test_copy_paths 验证 copyin、copyout 和 copyinstr 能访问跨 MMIO 数值的用户 VA。
 */
static void
test_copy_paths(void)
{
  int fds[2];
  char received = 0;
  char *pipe_source = (char *)PLIC;
  *pipe_source = 'Q';

  if(pipe(fds) < 0)
    fail("pipe failed");
  if(write(fds[1], pipe_source, 1) != 1)
    fail("copyin above PLIC failed");
  if(read(fds[0], &received, 1) != 1 || received != 'Q')
    fail("pipe payload mismatch");
  close(fds[0]);
  close(fds[1]);

  char *path = (char *)UART0;
  strcpy(path, "/README");
  int fd = open(path, O_RDONLY);
  if(fd < 0)
    fail("copyinstr above UART failed");

  char *copyout_target = (char *)(PLIC + PGSIZE);
  if(read(fd, copyout_target, 4) != 4)
    fail("copyout above PLIC failed");
  close(fd);
}

/**
 * test_cow 验证高地址用户页在 fork 后仍遵守 COW 隔离。
 */
static void
test_cow(void)
{
  volatile char *value = (volatile char *)CLINT;
  *value = 'A';

  int pid = fork();
  if(pid < 0)
    fail("cow fork failed");
  if(pid == 0){
    *value = 'B';
    if(*value != 'B')
      exit(2);
    exit(0);
  }

  int status = -1;
  wait(&status);
  if(status != 0 || *value != 'A')
    fail("high VA COW isolation");
}

/**
 * test_mmap 验证 mmap 可以放置在 UART 数值地址之后并由缺页路径建立别名。
 */
static void
test_mmap(void)
{
  int fd = open("/README", O_RDONLY);
  if(fd < 0)
    fail("mmap source open failed");

  char *mapped = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  if(mapped == (char *)-1)
    fail("mmap above UART failed");
  if((uint64)mapped <= VIRTIO0 || (uint64)mapped >= USERMAX)
    fail("mmap address outside expected range");

  volatile char first = mapped[0];
  (void)first;
  require_present((uint64)mapped);

  if(munmap(mapped, PGSIZE) < 0)
    fail("munmap above UART failed");
  close(fd);
}

/**
 * test_exec 验证 exec 的 path、argv 数组和参数字符串都可以来自高地址用户页。
 */
static void
test_exec(void)
{
  int pid = fork();
  if(pid < 0)
    fail("exec fork failed");
  if(pid == 0){
    char *path = (char *)(UART0 + 128);
    char *argument = (char *)(UART0 + 192);
    char **arguments = (char **)(PLIC + 2 * PGSIZE);

    strcpy(path, XV6_BIN_PATH("echo"));
    strcpy(argument, "alias-exec");
    arguments[0] = path;
    arguments[1] = argument;
    arguments[2] = 0;
    exec(path, arguments);
    exit(3);
  }

  int status = -1;
  wait(&status);
  if(status != 0)
    fail("exec with high VA arguments failed");
}

/**
 * test_direct_map_crossing 验证用户 VA 可以越过 KERNBASE 数值。
 *
 * 该页在用户页表中位于 2 GiB 附近，但进程内核页表的同一低地址仍是物理
 * direct map；copyin/copyout 必须通过高规范半区 alias 才能访问用户物理页。
 */
static void
test_direct_map_crossing(void)
{
  grow_to(KERNBASE + 3 * PGSIZE);

  char *source = (char *)KERNBASE;
  *source = 'D';
  require_present(KERNBASE);

  int fds[2];
  char received = 0;
  if(pipe(fds) < 0)
    fail("direct-map pipe failed");
  if(write(fds[1], source, 1) != 1)
    fail("copyin above KERNBASE failed");
  if(read(fds[0], &received, 1) != 1 || received != 'D')
    fail("direct-map pipe payload mismatch");
  close(fds[0]);
  close(fds[1]);

  int fd = open("/README", O_RDONLY);
  if(fd < 0)
    fail("direct-map copyout source failed");
  char *target = (char *)(KERNBASE + PGSIZE);
  if(read(fd, target, 4) != 4)
    fail("copyout above KERNBASE failed");
  close(fd);
  require_present(KERNBASE + PGSIZE);
}

/**
 * run_worker 在一个短生命周期进程中完成地址窗口功能验证。
 */
static void
run_worker(void)
{
  uint64 oldbrk = (uint64)sbrk(0);
  uint64 target = VIRTIO0 + 3 * PGSIZE;
  if(oldbrk >= target)
    fail("unexpected initial break");
  grow_to(target);

  uint64 boundaries[] = {CLINT, PLIC, UART0, VIRTIO0};
  int boundary_count = sizeof(boundaries) / sizeof(boundaries[0]);
  for(int i = 0; i < boundary_count; i++){
    volatile char *address = (volatile char *)boundaries[i];
    *address = (char)(0x30 + i);
    if(*address != (char)(0x30 + i))
      fail("numeric MMIO user access mismatch");
    require_present(boundaries[i]);
  }

  test_copy_paths();
  test_cow();
  test_mmap();
  test_exec();
  test_direct_map_crossing();

  if(memsnapshot(MEMVIZ_VIEW_KERNEL, &snapshot) < 0)
    fail("kernel snapshot failed");
  if(snapshot.user_limit != TRAPFRAME || snapshot.user_limit != USERMAX ||
     snapshot.user_mirror_start != KUSERBASE ||
     snapshot.user_mirror_end != KUSERADDR(snapshot.process_size) ||
     KUSERBASE != 0xffffffc000000000ULL || KUSEREND != KUSERADDR(USERMAX))
    fail("Sv39 alias layout mismatch");

  if(vaquery(USERMAX, &query) != -1)
    fail("USERMAX boundary accepted");

  printf("addresswindowtest: worker OK\n");
}

/**
 * main 验证功能路径，并在 worker 回收后检查页面和页表资源是否归还。
 */
int
main(int argc, char **argv)
{
  (void)argv;
  if(argc != 1)
    exit(2);

  uint64 before = free_pages();
  int pid = fork();
  if(pid < 0)
    fail("worker fork failed");
  if(pid == 0){
    run_worker();
    exit(0);
  }

  int status = -1;
  wait(&status);
  if(status != 0)
    fail("worker failed");
  if(free_pages() != before)
    fail("worker leaked pages");

  printf("addresswindowtest: OK\n");
  exit(0);
}
