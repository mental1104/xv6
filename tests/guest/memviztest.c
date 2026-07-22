#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/riscv.h"
#include "kernel/memviz.h"
#include "user/user.h"

static struct memviz_snapshot before;
static struct memviz_snapshot after_alloc;
static struct memviz_snapshot after_free;

/**
 * fail 输出失败原因并以非零状态终止测试。
 *
 * @param message 稳定的失败描述。
 */
static void
fail(char *message)
{
  printf("memviztest: FAIL: %s\n", message);
  exit(1);
}

/**
 * test_user_snapshot 验证用户栈、动态边界和物理计数的基本不变量。
 */
static void
test_user_snapshot(void)
{
  if(memsnapshot(99, &before) != -1)
    fail("invalid view accepted");
  if(memsnapshot(MEMVIZ_VIEW_USER, &before) < 0)
    fail("user snapshot syscall");
  if(!before.user_stack_valid)
    fail("user stack invalid");
  if(before.stack_used + before.stack_free != PGSIZE)
    fail("user stack accounting");
  if(before.dynamic_start != before.stack_top)
    fail("dynamic start mismatch");
  if(before.process_size < before.dynamic_start)
    fail("process size below dynamic start");
  if(before.free_pages + before.used_pages != before.total_pages)
    fail("physical total in user view");

  printf("memviztest: user invariants OK\n");
}

/**
 * test_physical_snapshot 验证 cell、CPU freelist 与全局页数相互一致。
 */
static void
test_physical_snapshot(void)
{
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &before) < 0)
    fail("physical snapshot syscall");

  uint64 cell_total = 0;
  uint64 cell_free = 0;
  for(int i = 0; i < MEMVIZ_CELLS; i++){
    if(before.physical[i].free_pages > before.physical[i].total_pages)
      fail("cell free exceeds total");
    cell_total += before.physical[i].total_pages;
    cell_free += before.physical[i].free_pages;
  }
  if(cell_total != before.total_pages)
    fail("cell total mismatch");
  if(cell_free != before.free_pages)
    fail("cell free mismatch");

  uint64 cpu_free = 0;
  for(int i = 0; i < NCPU; i++)
    cpu_free += before.cpu_free_pages[i];
  if(cpu_free != before.free_pages)
    fail("CPU freelist mismatch");

  printf("memviztest: physical invariants OK\n");
}

/**
 * test_allocate_and_release 验证触页会消耗物理页，缩容后页面会归还 kalloc。
 */
static void
test_allocate_and_release(void)
{
  const int pages = 4;
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &before) < 0)
    fail("baseline physical snapshot");

  char *base = sbrk(pages * PGSIZE);
  if(base == (char *)-1)
    fail("sbrk allocate");
  for(int i = 0; i < pages; i++)
    base[i * PGSIZE] = (char)i;

  if(memsnapshot(MEMVIZ_VIEW_PHYS, &after_alloc) < 0)
    fail("allocated physical snapshot");
  if(after_alloc.free_pages + pages > before.free_pages)
    fail("touched pages did not reduce free memory");

  if(sbrk(-pages * PGSIZE) == (char *)-1)
    fail("sbrk release");
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &after_free) < 0)
    fail("released physical snapshot");
  if(after_free.free_pages != before.free_pages)
    fail("released pages did not return to kalloc");

  printf("memviztest: allocate/release OK\n");
}

/**
 * test_kernel_snapshot 验证当前内核栈和固定映射边界可观察。
 */
static void
test_kernel_snapshot(void)
{
  if(memsnapshot(MEMVIZ_VIEW_KERNEL, &before) < 0)
    fail("kernel snapshot syscall");
  if(!before.kernel_stack_valid)
    fail("kernel stack invalid");
  if(before.kernel_stack_used + before.kernel_stack_free != PGSIZE)
    fail("kernel stack accounting");
  if(before.kernel_text_start >= before.kernel_text_end)
    fail("kernel text range");
  if(before.kalloc_start >= before.kalloc_end)
    fail("kalloc range");
  if(before.user_mirror_end != before.process_size)
    fail("user mirror range");

  printf("memviztest: kernel invariants OK\n");
}

/**
 * run_named 运行一个具名检查，便于 CI 将失败定位到单一不变量组。
 *
 * @param name user、phys、alloc 或 kernel。
 * @return 名称有效返回 0，否则返回 -1。
 */
static int
run_named(char *name)
{
  if(strcmp(name, "user") == 0)
    test_user_snapshot();
  else if(strcmp(name, "phys") == 0)
    test_physical_snapshot();
  else if(strcmp(name, "alloc") == 0)
    test_allocate_and_release();
  else if(strcmp(name, "kernel") == 0)
    test_kernel_snapshot();
  else
    return -1;
  return 0;
}

/**
 * main 默认运行完整测试；传入一个名称时只运行对应检查。
 */
int
main(int argc, char **argv)
{
  if(argc == 1){
    test_user_snapshot();
    test_physical_snapshot();
    test_allocate_and_release();
    test_kernel_snapshot();
  } else if(argc == 2){
    if(run_named(argv[1]) < 0){
      fprintf(2, "usage: memviztest [user|phys|alloc|kernel]\n");
      exit(1);
    }
  } else {
    fprintf(2, "usage: memviztest [user|phys|alloc|kernel]\n");
    exit(1);
  }

  printf("memviztest: OK\n");
  exit(0);
}
