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
  if(before.user_sp < before.stack_bottom || before.user_sp > before.stack_top)
    fail("user sp outside stack bounds");
  if(before.dynamic_start != before.stack_top)
    fail("dynamic start mismatch");
  if(before.process_size < before.dynamic_start)
    fail("process size below dynamic start");
  if(before.process_size == before.dynamic_start){
    uint64 dynamic_pages = 0;
    if(dynamic_pages != 0)
      fail("empty dynamic extent accounting");
  }
  if(before.free_pages + before.used_pages != before.total_pages)
    fail("physical total in user view");

  printf("memviztest: user invariants OK\n");
}

/**
 * test_dynamic_extent 验证 sbrk 后 p->sz 从 dynamic_start 向高地址增长。
 */
static void
test_dynamic_extent(void)
{
  const int pages = 2;
  if(memsnapshot(MEMVIZ_VIEW_USER, &before) < 0)
    fail("dynamic baseline snapshot");

  char *base = sbrk(pages * PGSIZE);
  if(base == (char *)-1)
    fail("dynamic sbrk allocate");
  for(int i = 0; i < pages; i++)
    base[i * PGSIZE] = (char)i;

  if(memsnapshot(MEMVIZ_VIEW_USER, &after_alloc) < 0)
    fail("dynamic allocated snapshot");
  if(after_alloc.process_size <= after_alloc.dynamic_start)
    fail("dynamic extent did not grow upward");
  if((after_alloc.process_size - after_alloc.dynamic_start) / PGSIZE < pages)
    fail("dynamic extent page count");

  if(sbrk(-pages * PGSIZE) == (char *)-1)
    fail("dynamic sbrk release");

  printf("memviztest: dynamic extent OK\n");
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
    char state;
    if(before.physical[i].free_pages == 0)
      state = '#';
    else if(before.physical[i].free_pages == before.physical[i].total_pages)
      state = '.';
    else
      state = ':';
    if(state != '#' && state != '.' && state != ':')
      fail("invalid cell state");
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

  uint64 covered = 0;
  for(int cell = 0; cell < MEMVIZ_CELLS; cell++){
    uint64 expected = 0;
    for(uint64 page = 0; page < before.total_pages; page++){
      if((page * MEMVIZ_CELLS) / before.total_pages == (uint64)cell)
        expected++;
    }
    if(before.physical[cell].total_pages != expected)
      fail("cell coverage mismatch");
    covered += expected;
  }
  if(covered != before.total_pages)
    fail("cell coverage total mismatch");

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
 * test_pagetable_snapshot 验证页表观察条目能连接 VA、PA 和 kalloc 池。
 */
static void
test_pagetable_snapshot(void)
{
  if(memsnapshot(MEMVIZ_VIEW_PAGETABLE, &before) < 0)
    fail("pagetable snapshot syscall");
  if(before.user_pagetable == 0 || before.kernel_pagetable == 0)
    fail("pagetable roots missing");
  if(before.pagetable_entry_count == 0 ||
     before.pagetable_entry_count > MEMVIZ_PTE_ENTRIES)
    fail("pagetable entry count");
  if(before.pagetable_usage_count == 0 ||
     before.pagetable_usage_count > MEMVIZ_PT_USAGE_PAGES)
    fail("pagetable usage count");

  int user_stack = 0;
  int guard_inaccessible = 0;
  int user_mirror = 0;
  int kernel_stack = 0;
  int kalloc_backed = 0;

  for(int i = 0; i < (int)before.pagetable_entry_count; i++){
    struct memviz_pte_entry *entry = &before.pagetable_entries[i];
    if(entry->space != MEMVIZ_PTE_SPACE_USER &&
       entry->space != MEMVIZ_PTE_SPACE_KERNEL)
      fail("pagetable entry space");
    if(entry->present && (entry->flags & PTE_V) == 0)
      fail("present entry without PTE_V");
    if(entry->levels[0].level != 2 || entry->levels[1].level != 1 ||
       entry->levels[2].level != 0)
      fail("pagetable level order");
    for(int level = 0; level < 3; level++){
      if(entry->levels[level].index < 0 || entry->levels[level].index > 511)
        fail("pagetable level index");
      if(entry->levels[level].present &&
         (entry->levels[level].flags & PTE_V) == 0)
        fail("pagetable level without PTE_V");
    }
    if(entry->present && !entry->levels[2].present)
      fail("leaf mapping without L0");

    if(entry->role == MEMVIZ_PTE_ROLE_USER_STACK && entry->present)
      user_stack = 1;
    if(entry->role == MEMVIZ_PTE_ROLE_GUARD){
      if(entry->present && (entry->flags & PTE_U))
        fail("guard keeps PTE_U");
      guard_inaccessible = 1;
    }
    if(entry->role == MEMVIZ_PTE_ROLE_USER_MIRROR && entry->present){
      if(entry->flags & PTE_U)
        fail("user mirror keeps PTE_U");
      user_mirror = 1;
    }
    if(entry->role == MEMVIZ_PTE_ROLE_KERNEL_STACK && entry->present)
      kernel_stack = 1;
    if(entry->present && entry->pa >= before.kalloc_start &&
       entry->pa < before.kalloc_end)
      kalloc_backed = 1;
  }

  if(!user_stack)
    fail("user stack pte missing");
  if(!guard_inaccessible)
    fail("guard inaccessible pte missing");
  if(!user_mirror)
    fail("user mirror pte missing");
  if(!kernel_stack)
    fail("kernel stack pte missing");
  if(!kalloc_backed)
    fail("no pte reaches kalloc pool");

  for(int i = 0; i < (int)before.pagetable_usage_count; i++){
    struct memviz_pt_usage_page *page = &before.pagetable_usage[i];
    if(page->space != MEMVIZ_PTE_SPACE_USER &&
       page->space != MEMVIZ_PTE_SPACE_KERNEL)
      fail("pagetable usage space");
    if(page->level < 0 || page->level > 2)
      fail("pagetable usage level");
    if(page->total_entries != 512 || page->used_entries > page->total_entries)
      fail("pagetable usage entries");

    uint64 cell_total = 0;
    uint64 cell_used = 0;
    for(int cell = 0; cell < MEMVIZ_PT_USAGE_CELLS; cell++){
      struct memviz_pt_usage_cell *usage_cell = &page->cells[cell];
      if(usage_cell->used_entries > usage_cell->total_entries)
        fail("pagetable usage cell used");
      cell_total += usage_cell->total_entries;
      cell_used += usage_cell->used_entries;
    }
    if(cell_total != page->total_entries || cell_used != page->used_entries)
      fail("pagetable usage cell total");
  }

  printf("memviztest: pagetable invariants OK\n");
}

/**
 * run_named 运行一个具名检查，便于 CI 将失败定位到单一不变量组。
 *
 * @param name user、phys、alloc、kernel 或 pagetable。
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
  else if(strcmp(name, "pagetable") == 0)
    test_pagetable_snapshot();
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
    test_dynamic_extent();
    test_physical_snapshot();
    test_allocate_and_release();
    test_kernel_snapshot();
    test_pagetable_snapshot();
  } else if(argc == 2){
    if(run_named(argv[1]) < 0){
      fprintf(2, "usage: memviztest [user|phys|alloc|kernel|pagetable]\n");
      exit(1);
    }
  } else {
    fprintf(2, "usage: memviztest [user|phys|alloc|kernel|pagetable]\n");
    exit(1);
  }

  printf("memviztest: OK\n");
  exit(0);
}
