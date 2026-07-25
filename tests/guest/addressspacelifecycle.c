#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"
#include "kernel/memviz.h"
#include "user/user.h"

/** addresswindowtest.c 经构建规则重命名后的原始入口。 */
int addresswindowtest_original_main(int argc, char **argv);

static struct memviz_snapshot lifecycle_snapshot;
static struct memviz_snapshot lifecycle_physical_before;
static struct memviz_snapshot lifecycle_physical_live;
static struct memviz_snapshot lifecycle_physical_after;

/** 地址空间闭环中由子进程主动采集并交给父进程的最小证据。 */
struct address_space_report {
  int pid;
  int value;
  uint64 process_size;
  uint64 dynamic_page_count;
  uint64 dynamic_cow_pages;
  uint64 dynamic_lazy_pages;
  uint64 user_pagetable;
  uint64 kernel_pagetable;
  uint64 trapframe_flags;
  uint64 trampoline_flags;
  struct memviz_va_query mapping;
};

/** 输出稳定失败原因并以非零状态终止测试。 */
static void
lifecycle_fail(char *message)
{
  printf("addresswindowtest: FAIL: address-space %s\n", message);
  exit(1);
}

/**
 * 从管道完整读取一个固定大小对象。
 *
 * @param fd 可读管道描述符。
 * @param buffer 接收数据的缓冲区。
 * @param size 需要读取的字节数。
 * @return 完整读取返回 0；提前 EOF 或读取失败返回 -1。
 */
static int
read_exact(int fd, void *buffer, int size)
{
  char *cursor = buffer;
  int total = 0;

  while(total < size){
    int count = read(fd, cursor + total, size - total);
    if(count <= 0)
      return -1;
    total += count;
  }
  return 0;
}

/**
 * 向管道完整写入一个固定大小对象。
 *
 * @param fd 可写管道描述符。
 * @param buffer 待发送数据。
 * @param size 需要写入的字节数。
 * @return 完整写入返回 0；写入失败返回 -1。
 */
static int
write_exact(int fd, const void *buffer, int size)
{
  const char *cursor = buffer;
  int total = 0;

  while(total < size){
    int count = write(fd, cursor + total, size - total);
    if(count <= 0)
      return -1;
    total += count;
  }
  return 0;
}

/**
 * 采集当前进程地址空间和指定虚拟地址的紧凑证据。
 *
 * @param report 接收进程、页表、动态页和 PTE 状态。
 * @param address 当前进程中需要追踪的可读地址。
 * @return memsnapshot() 与 vaquery() 都成功时返回 0，否则返回 -1。
 */
static int
capture_report(struct address_space_report *report, char *address)
{
  memset(report, 0, sizeof(*report));
  if(memsnapshot(MEMVIZ_VIEW_USER, &lifecycle_snapshot) < 0)
    return -1;
  if(vaquery((uint64)address, &report->mapping) < 0)
    return -1;

  report->pid = getpid();
  report->value = address[0];
  report->process_size = lifecycle_snapshot.process_size;
  report->dynamic_page_count = lifecycle_snapshot.dynamic_page_count;
  report->dynamic_cow_pages = lifecycle_snapshot.dynamic_cow_pages;
  report->dynamic_lazy_pages = lifecycle_snapshot.dynamic_lazy_pages;
  report->user_pagetable = lifecycle_snapshot.user_pagetable;
  report->kernel_pagetable = lifecycle_snapshot.kernel_pagetable;
  report->trapframe_flags = lifecycle_snapshot.trapframe_flags;
  report->trampoline_flags = lifecycle_snapshot.trampoline_flags;
  return 0;
}

/**
 * 验证建立、fork 共享、COW 分离、惰性扩展和 wait 回收的地址空间闭环。
 *
 * 子进程只观察自己的页表，再通过 pipe 交付报告；父进程不跨生命周期读取其他
 * struct proc。release pipe 让目标保持稳定，直到父进程完成 COW 前后比较。
 */
static void
test_address_space_lifecycle(void)
{
  int report_pipe[2];
  int release_pipe[2];
  struct address_space_report child_before;
  struct address_space_report child_after;
  struct memviz_va_query parent_mapping;

  if(memsnapshot(MEMVIZ_VIEW_PHYS, &lifecycle_physical_before) < 0)
    lifecycle_fail("initial physical snapshot");

  if(pipe(report_pipe) < 0 || pipe(release_pipe) < 0)
    lifecycle_fail("pipe creation");

  char *shared = sbrk(PGSIZE);
  if(shared == (char *)-1)
    lifecycle_fail("parent page allocation");
  shared[0] = 11;

  if(memsnapshot(MEMVIZ_VIEW_USER, &lifecycle_snapshot) < 0)
    lifecycle_fail("parent snapshot");
  uint64 parent_user_pagetable = lifecycle_snapshot.user_pagetable;
  uint64 parent_kernel_pagetable = lifecycle_snapshot.kernel_pagetable;
  uint64 parent_process_size = lifecycle_snapshot.process_size;
  uint64 parent_dynamic_pages = lifecycle_snapshot.dynamic_page_count;

  int pid = fork();
  if(pid < 0)
    lifecycle_fail("fork");
  if(pid == 0){
    close(report_pipe[0]);
    close(release_pipe[1]);

    if(capture_report(&child_before, shared) < 0 ||
       write_exact(report_pipe[1], &child_before, sizeof(child_before)) < 0)
      exit(1);

    shared[0] = 22;
    char *extra = sbrk(2 * PGSIZE);
    if(extra == (char *)-1)
      exit(1);
    extra[0] = 33;

    if(capture_report(&child_after, shared) < 0 ||
       write_exact(report_pipe[1], &child_after, sizeof(child_after)) < 0)
      exit(1);
    close(report_pipe[1]);

    char token;
    if(read_exact(release_pipe[0], &token, 1) < 0)
      exit(1);
    close(release_pipe[0]);
    exit(0);
  }

  close(report_pipe[1]);
  close(release_pipe[0]);
  if(read_exact(report_pipe[0], &child_before, sizeof(child_before)) < 0 ||
     read_exact(report_pipe[0], &child_after, sizeof(child_after)) < 0)
    lifecycle_fail("child reports");
  close(report_pipe[0]);

  if(vaquery((uint64)shared, &parent_mapping) < 0)
    lifecycle_fail("parent mapping query");

  if(child_before.pid != pid || child_after.pid != pid)
    lifecycle_fail("child pid mismatch");
  if(child_before.user_pagetable == parent_user_pagetable ||
     child_before.kernel_pagetable == parent_kernel_pagetable)
    lifecycle_fail("page-table roots shared");
  if(child_before.process_size != parent_process_size)
    lifecycle_fail("fork size mismatch");
  if(child_after.process_size != parent_process_size + 2 * PGSIZE ||
     child_after.dynamic_page_count != parent_dynamic_pages + 2)
    lifecycle_fail("child growth mismatch");
  if(child_after.dynamic_lazy_pages != child_before.dynamic_lazy_pages + 1)
    lifecycle_fail("untouched child page not lazy");

  uint64 cow_required = PTE_V | PTE_R | PTE_U | PTE_COW;
  if(!child_before.mapping.present ||
     (child_before.mapping.flags & cow_required) != cow_required ||
     (child_before.mapping.flags & PTE_W))
    lifecycle_fail("child pre-write COW permissions");
  if(!parent_mapping.present ||
     (parent_mapping.flags & cow_required) != cow_required ||
     (parent_mapping.flags & PTE_W))
    lifecycle_fail("parent COW permissions");
  if(child_before.mapping.pa != parent_mapping.pa)
    lifecycle_fail("fork physical page not shared");

  uint64 private_required = PTE_V | PTE_R | PTE_W | PTE_U;
  if(!child_after.mapping.present ||
     (child_after.mapping.flags & private_required) != private_required ||
     (child_after.mapping.flags & PTE_COW))
    lifecycle_fail("child private permissions");
  if(child_after.mapping.pa == parent_mapping.pa)
    lifecycle_fail("COW physical page not split");
  if(shared[0] != 11 || child_before.value != 11 || child_after.value != 22)
    lifecycle_fail("parent and child values not isolated");

  uint64 trapframe_required = PTE_V | PTE_R | PTE_W;
  if((child_after.trapframe_flags & trapframe_required) != trapframe_required ||
     (child_after.trapframe_flags & (PTE_X | PTE_U)))
    lifecycle_fail("trapframe permissions");
  uint64 trampoline_required = PTE_V | PTE_R | PTE_X;
  if((child_after.trampoline_flags & trampoline_required) !=
       trampoline_required ||
     (child_after.trampoline_flags & (PTE_W | PTE_U)))
    lifecycle_fail("trampoline permissions");

  printf("ADDRESS SPACE stage=parent pid=%d root=%p size=%p va=%p pa=%p value=%d\n",
         getpid(), parent_user_pagetable, parent_process_size,
         parent_mapping.va, parent_mapping.pa, shared[0]);
  printf("ADDRESS SPACE stage=child-before pid=%d root=%p size=%p va=%p pa=%p flags=%p value=%d\n",
         child_before.pid, child_before.user_pagetable,
         child_before.process_size, child_before.mapping.va,
         child_before.mapping.pa, child_before.mapping.flags,
         child_before.value);
  printf("ADDRESS SPACE stage=child-after pid=%d root=%p size=%p va=%p pa=%p flags=%p value=%d lazy=%d\n",
         child_after.pid, child_after.user_pagetable,
         child_after.process_size, child_after.mapping.va,
         child_after.mapping.pa, child_after.mapping.flags,
         child_after.value, (int)child_after.dynamic_lazy_pages);

  if(memsnapshot(MEMVIZ_VIEW_PHYS, &lifecycle_physical_live) < 0)
    lifecycle_fail("live physical snapshot");

  char token = 'x';
  if(write_exact(release_pipe[1], &token, 1) < 0)
    lifecycle_fail("release child");
  close(release_pipe[1]);

  int status = -1;
  if(wait(&status) != pid || status != 0)
    lifecycle_fail("wait child");
  if(wait(0) != -1)
    lifecycle_fail("reaped child remains waitable");

  if(memsnapshot(MEMVIZ_VIEW_PHYS, &lifecycle_physical_after) < 0)
    lifecycle_fail("reclaimed physical snapshot");
  if(lifecycle_physical_after.free_pages <= lifecycle_physical_live.free_pages)
    lifecycle_fail("child resources not reclaimed");

  if(sbrk(-PGSIZE) == (char *)-1)
    lifecycle_fail("parent cleanup");
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &lifecycle_physical_after) < 0)
    lifecycle_fail("final physical snapshot");
  if(lifecycle_physical_after.free_pages != lifecycle_physical_before.free_pages)
    lifecycle_fail("final page leak");

  printf("ADDRESS SPACE target pid=%d state=REAPED pages_returned=yes\n", pid);
  printf("ADDRESS SPACE RESULT PASS\n");
}

/**
 * 先运行地址空间生命周期闭环，再委托原 addresswindowtest 入口执行既有实验。
 *
 * paging 聚焦入口保持原语义，避免一条局部命令意外扩大运行范围。
 */
int
main(int argc, char **argv)
{
  if(argc == 1)
    test_address_space_lifecycle();
  return addresswindowtest_original_main(argc, argv);
}
