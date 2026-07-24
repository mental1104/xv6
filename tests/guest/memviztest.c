#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"
#include "kernel/fcntl.h"
#include "kernel/memviz.h"
#include "user/user.h"

static struct memviz_snapshot before;
static struct memviz_snapshot after_alloc;
static struct memviz_snapshot after_free;
static char render_output[32768];

/** 输出失败原因并以非零状态终止测试。 */
static void
fail(char *message)
{
  printf("memviztest: FAIL: %s\n", message);
  exit(1);
}

static int
text_contains(char *text, char *pattern)
{
  for(int i = 0; text[i] != 0; i++){
    int j = 0;
    while(pattern[j] != 0 && text[i + j] == pattern[j])
      j++;
    if(pattern[j] == 0)
      return 1;
  }
  return 0;
}

/** 将正整数 PID 写成十进制字符串。 */
static void
format_pid(int pid, char *buffer)
{
  char reversed[16];
  int count = 0;
  do {
    reversed[count++] = '0' + pid % 10;
    pid /= 10;
  } while(pid > 0);
  for(int i = 0; i < count; i++)
    buffer[i] = reversed[count - 1 - i];
  buffer[count] = 0;
}

static uint64
cell_page_count(uint64 total_pages, int cell)
{
  uint64 first = ((uint64)cell * total_pages + MEMVIZ_CELLS - 1) /
                 MEMVIZ_CELLS;
  uint64 end = ((uint64)(cell + 1) * total_pages + MEMVIZ_CELLS - 1) /
               MEMVIZ_CELLS;
  return end - first;
}

static void
check_dynamic_state_totals(struct memviz_snapshot *snapshot)
{
  if(snapshot->dynamic_state_cell_count > MEMVIZ_USER_STATE_CELLS)
    fail("dynamic state cell count");
  if(snapshot->dynamic_page_count == 0 &&
     snapshot->dynamic_state_cell_count != 0)
    fail("empty dynamic state keeps cells");
  if(snapshot->dynamic_page_count > 0 &&
     snapshot->dynamic_state_cell_count == 0)
    fail("dynamic state cells missing");

  uint64 total = 0;
  uint64 resident = 0;
  uint64 cow = 0;
  uint64 lazy = 0;
  uint64 mmap_pages = 0;
  for(int i = 0; i < (int)snapshot->dynamic_state_cell_count; i++){
    struct memviz_user_state_cell *cell = &snapshot->dynamic_state[i];
    uint64 classified = cell->resident_pages + cell->cow_pages +
                        cell->lazy_pages + cell->mmap_pages;
    if(classified != cell->total_pages)
      fail("dynamic cell classification");
    total += cell->total_pages;
    resident += cell->resident_pages;
    cow += cell->cow_pages;
    lazy += cell->lazy_pages;
    mmap_pages += cell->mmap_pages;
  }

  if(total != snapshot->dynamic_page_count)
    fail("dynamic cell page total");
  if(resident != snapshot->dynamic_resident_pages ||
     cow != snapshot->dynamic_cow_pages ||
     lazy != snapshot->dynamic_lazy_pages ||
     mmap_pages != snapshot->dynamic_mmap_pages)
    fail("dynamic global state totals");
  if(resident + cow + lazy + mmap_pages != snapshot->dynamic_page_count)
    fail("dynamic state partition");
}

static int
capture_user_render(int plain)
{
  int fds[2];
  if(pipe(fds) < 0)
    fail("render pipe");

  int pid = fork();
  if(pid < 0)
    fail("render fork");
  if(pid == 0){
    close(fds[0]);
    close(1);
    if(dup(fds[1]) != 1)
      exit(1);
    close(fds[1]);

    char *plain_argv[] = { "memviz", "user", "--plain", 0 };
    char *color_argv[] = { "memviz", "user", 0 };
    exec("memviz", plain ? plain_argv : color_argv);
    exit(1);
  }

  close(fds[1]);
  int total = 0;
  while(total < (int)sizeof(render_output) - 1){
    int count = read(fds[0], render_output + total,
                     sizeof(render_output) - 1 - total);
    if(count < 0)
      fail("render read");
    if(count == 0)
      break;
    total += count;
  }
  close(fds[0]);
  render_output[total] = 0;

  int status = -1;
  if(wait(&status) != pid || status != 0)
    fail("memviz user execution");
  return total;
}

/** 执行真实 memviz user --pid 命令并捕获其 stdout。 */
static int
capture_user_render_pid(int target_pid)
{
  int fds[2];
  if(pipe(fds) < 0)
    fail("pid render pipe");

  int pid = fork();
  if(pid < 0)
    fail("pid render fork");
  if(pid == 0){
    char pid_text[16];
    format_pid(target_pid, pid_text);
    close(fds[0]);
    close(1);
    if(dup(fds[1]) != 1)
      exit(1);
    close(fds[1]);

    char *argv[] = { "memviz", "user", "--pid", pid_text, "--plain", 0 };
    exec("memviz", argv);
    exit(1);
  }

  close(fds[1]);
  int total = 0;
  while(total < (int)sizeof(render_output) - 1){
    int count = read(fds[0], render_output + total,
                     sizeof(render_output) - 1 - total);
    if(count < 0)
      fail("pid render read");
    if(count == 0)
      break;
    total += count;
  }
  close(fds[0]);
  render_output[total] = 0;

  int status = -1;
  if(wait(&status) != pid || status != 0)
    fail("memviz pid execution");
  return total;
}

/** 多核下目标可能仍在收尾运行，短暂重试直到进入可稳定采样状态。 */
static int
snapshot_pid_retry(int pid, int view, struct memviz_snapshot *snapshot)
{
  for(int attempt = 0; attempt < 100; attempt++){
    if(memsnapshot_pid(pid, view, snapshot) == 0)
      return 0;
    sleep(1);
  }
  return -1;
}

static void
test_user_snapshot(void)
{
  if(memsnapshot(99, &before) != -1)
    fail("invalid view accepted");
  if(memsnapshot(MEMVIZ_VIEW_USER, &before) < 0)
    fail("user snapshot syscall");
  if(before.process_pid != getpid())
    fail("current process pid missing");
  if(before.user_limit != USERMAX)
    fail("user limit mismatch");
  if(before.maxva != MAXVA)
    fail("MAXVA mismatch");
  if(before.trapframe != TRAPFRAME || before.trapframe != before.user_limit)
    fail("trapframe VA mismatch");
  if(before.trampoline != TRAMPOLINE)
    fail("trampoline VA mismatch");
  if(before.trapframe + PGSIZE != before.trampoline ||
     before.trampoline + PGSIZE != before.maxva)
    fail("top fixed pages are not adjacent");

  if(before.trapframe_pa == 0 || before.trampoline_pa == 0)
    fail("top fixed page PA missing");
  if((before.trapframe_pa % PGSIZE) != 0 ||
     (before.trampoline_pa % PGSIZE) != 0)
    fail("top fixed page PA alignment");
  if(before.trapframe_pa == before.trampoline_pa)
    fail("top fixed pages share PA");

  uint64 trapframe_required = PTE_V | PTE_R | PTE_W;
  if((before.trapframe_flags & trapframe_required) != trapframe_required)
    fail("trapframe PTE permissions");
  if(before.trapframe_flags & (PTE_X | PTE_U))
    fail("trapframe executable or user-accessible");

  uint64 trampoline_required = PTE_V | PTE_R | PTE_X;
  if((before.trampoline_flags & trampoline_required) != trampoline_required)
    fail("trampoline PTE permissions");
  if(before.trampoline_flags & (PTE_W | PTE_U))
    fail("trampoline writable or user-accessible");

  if(before.trapframe_used !=
     MEMVIZ_TRAPFRAME_SLOT_COUNT * sizeof(uint64))
    fail("trapframe used bytes");
  if(before.trapframe_used >= PGSIZE)
    fail("trapframe does not fit one page");
  if(before.uservec_offset > before.userret_offset ||
     before.userret_offset >= before.trampoline_used ||
     before.trampoline_used >= PGSIZE)
    fail("trampoline symbol order");

  if(!before.user_stack_valid)
    fail("user stack invalid");
  if(before.stack_used + before.stack_free != PGSIZE)
    fail("user stack accounting");
  if(before.user_sp < before.stack_bottom || before.user_sp > before.stack_top)
    fail("user sp outside stack bounds");
  if(before.trapframe_values[MEMVIZ_TF_SP] != before.user_sp)
    fail("trapframe SP snapshot mismatch");
  if(before.trapframe_values[MEMVIZ_TF_KERNEL_SATP] == 0 ||
     before.trapframe_values[MEMVIZ_TF_KERNEL_SP] == 0 ||
     before.trapframe_values[MEMVIZ_TF_KERNEL_TRAP] == 0)
    fail("trapframe kernel entry context missing");

  if(before.dynamic_start != before.stack_top)
    fail("dynamic start mismatch");
  if(before.process_size < before.dynamic_start)
    fail("process size below dynamic start");
  if(before.process_size > before.user_limit)
    fail("process size above user limit");
  check_dynamic_state_totals(&before);
  if(before.free_pages + before.used_pages != before.total_pages)
    fail("physical total in user view");

  printf("memviztest: user invariants OK\n");
}

static void
test_user_render(void)
{
  capture_user_render(1);
  if(!text_contains(render_output, "MAXVA"))
    fail("render MAXVA missing");
  if(!text_contains(render_output, "TRAMPOLINE / supervisor-only RX"))
    fail("render trampoline block missing");
  if(!text_contains(render_output, "TRAPFRAME / supervisor-only RW"))
    fail("render trapframe block missing");
  if(!text_contains(render_output, "ADDRESS-SPACE BREAK"))
    fail("render address gap break missing");
  if(!text_contains(render_output, "not drawn to scale"))
    fail("render scale warning missing");
  if(!text_contains(render_output, "DYNAMIC EXTENT / page states"))
    fail("render dynamic states missing");
  if(!text_contains(render_output, "# resident") ||
     !text_contains(render_output, "C COW") ||
     !text_contains(render_output, "L lazy") ||
     !text_contains(render_output, "M mmap"))
    fail("plain dynamic legend missing");
  if(!text_contains(render_output, "TRAMPOLINE PAGE DETAIL"))
    fail("render trampoline detail missing");
  if(!text_contains(render_output, "TRAPFRAME PAGE MEMBER ORDER"))
    fail("render trapframe detail missing");
  if(!text_contains(render_output, "name=kernel_satp") ||
     !text_contains(render_output, "name=t6"))
    fail("render trapframe member boundaries missing");
  if(!text_contains(render_output, "observed process pid="))
    fail("render process pid missing");

  capture_user_render(0);
  if(!text_contains(render_output, "\033[33m.\033[0m"))
    fail("COW yellow point missing");
  if(!text_contains(render_output, "\033[34m.\033[0m"))
    fail("lazy blue point missing");
  if(!text_contains(render_output, "\033[38;5;208m.\033[0m"))
    fail("mmap orange point missing");

  printf("memviztest: user renderer OK\n");
}

static void
test_target_pid_snapshot(void)
{
  if(memsnapshot_pid(0, MEMVIZ_VIEW_USER, &after_alloc) != -1 ||
     memsnapshot_pid(-1, MEMVIZ_VIEW_USER, &after_alloc) != -1)
    fail("invalid target pid accepted");
  if(memsnapshot_pid(getpid(), MEMVIZ_VIEW_USER, &after_alloc) < 0 ||
     after_alloc.process_pid != getpid())
    fail("explicit self snapshot");

  int ready[2];
  int release_pipe[2];
  if(pipe(ready) < 0 || pipe(release_pipe) < 0)
    fail("target pipes");

  int pid = fork();
  if(pid < 0)
    fail("target fork");
  if(pid == 0){
    close(ready[0]);
    close(release_pipe[1]);
    char *base = sbrk(2 * PGSIZE);
    if(base == (char *)-1)
      exit(1);
    base[0] = 7;
    if(write(ready[1], "R", 1) != 1)
      exit(1);
    close(ready[1]);
    char token;
    if(read(release_pipe[0], &token, 1) != 1)
      exit(1);
    close(release_pipe[0]);
    exit(0);
  }

  close(ready[1]);
  close(release_pipe[0]);
  char token;
  if(read(ready[0], &token, 1) != 1)
    fail("target ready");
  close(ready[0]);

  if(memsnapshot(MEMVIZ_VIEW_USER, &before) < 0)
    fail("target parent baseline");
  if(snapshot_pid_retry(pid, MEMVIZ_VIEW_USER, &after_alloc) < 0)
    fail("target pid snapshot");
  if(after_alloc.process_pid != pid)
    fail("target pid identity");
  if(after_alloc.process_size != before.process_size + 2 * PGSIZE)
    fail("target process size");
  if(after_alloc.dynamic_page_count != before.dynamic_page_count + 2)
    fail("target dynamic page count");
  check_dynamic_state_totals(&after_alloc);

  if(snapshot_pid_retry(pid, MEMVIZ_VIEW_KERNEL, &after_free) < 0)
    fail("target kernel snapshot");
  if(after_free.process_pid != pid || !after_free.kernel_stack_valid)
    fail("target saved kernel stack");
  if(snapshot_pid_retry(pid, MEMVIZ_VIEW_PAGETABLE, &after_free) < 0)
    fail("target pagetable snapshot");
  if(after_free.process_pid != pid || after_free.user_pagetable == 0)
    fail("target pagetable identity");

  capture_user_render_pid(pid);
  char pid_text[16];
  format_pid(pid, pid_text);
  if(!text_contains(render_output, "observed process pid=") ||
     !text_contains(render_output, pid_text))
    fail("target CLI pid output");

  if(write(release_pipe[1], "X", 1) != 1)
    fail("target release");
  close(release_pipe[1]);
  int status = -1;
  if(wait(&status) != pid || status != 0)
    fail("target child exit");
  if(memsnapshot_pid(pid, MEMVIZ_VIEW_USER, &after_free) != -1)
    fail("reaped target remains observable");

  printf("memviztest: target pid observation OK\n");
}

static void
test_dynamic_page_states(void)
{
  const int pages = 2;
  if(memsnapshot(MEMVIZ_VIEW_USER, &before) < 0)
    fail("page-state baseline snapshot");
  check_dynamic_state_totals(&before);

  char *base = sbrk(pages * PGSIZE);
  if(base == (char *)-1)
    fail("lazy sbrk allocate");
  if(memsnapshot(MEMVIZ_VIEW_USER, &after_alloc) < 0)
    fail("lazy snapshot");
  check_dynamic_state_totals(&after_alloc);
  if(after_alloc.dynamic_page_count != before.dynamic_page_count + pages)
    fail("lazy page count");
  if(after_alloc.dynamic_lazy_pages != before.dynamic_lazy_pages + pages)
    fail("untouched sbrk pages are not lazy");

  base[0] = 7;
  if(memsnapshot(MEMVIZ_VIEW_USER, &after_free) < 0)
    fail("resident snapshot");
  check_dynamic_state_totals(&after_free);
  if(after_free.dynamic_resident_pages !=
     after_alloc.dynamic_resident_pages + 1)
    fail("touched lazy page not resident");
  if(after_free.dynamic_lazy_pages + 1 != after_alloc.dynamic_lazy_pages)
    fail("touched lazy count did not decrease");

  if(sbrk(-pages * PGSIZE) == (char *)-1)
    fail("lazy sbrk release");

  char *path = "memviz-state-file";
  int fd = open(path, O_CREATE | O_RDWR);
  if(fd < 0)
    fail("mmap test open");
  if(write(fd, "x", 1) != 1)
    fail("mmap test write");

  char *mapped = mmap(0, pages * PGSIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE, fd, 0);
  if(mapped == (char *)-1)
    fail("mmap page-state map");
  if(memsnapshot(MEMVIZ_VIEW_USER, &after_alloc) < 0)
    fail("untouched mmap snapshot");
  check_dynamic_state_totals(&after_alloc);
  if(after_alloc.dynamic_mmap_pages != before.dynamic_mmap_pages + pages)
    fail("untouched mmap pages not orange class");

  mapped[0] ^= 1;
  if(memsnapshot(MEMVIZ_VIEW_USER, &after_free) < 0)
    fail("resident mmap snapshot");
  check_dynamic_state_totals(&after_free);
  if(after_free.dynamic_mmap_pages != before.dynamic_mmap_pages + pages)
    fail("resident mmap page lost mmap class");

  int pid = fork();
  if(pid < 0)
    fail("page-state fork");
  if(pid == 0){
    if(memsnapshot(MEMVIZ_VIEW_USER, &before) < 0)
      exit(1);
    check_dynamic_state_totals(&before);
    if(before.dynamic_cow_pages < 1)
      exit(1);
    if(before.dynamic_mmap_pages < 1)
      exit(1);
    exit(0);
  }

  int status = -1;
  if(wait(&status) != pid || status != 0)
    fail("child COW state");
  if(memsnapshot(MEMVIZ_VIEW_USER, &after_free) < 0)
    fail("parent COW snapshot");
  check_dynamic_state_totals(&after_free);
  if(after_free.dynamic_cow_pages < 1)
    fail("parent mmap leaf not promoted to COW");
  if(after_free.dynamic_mmap_pages < 1)
    fail("untouched mmap page missing after fork");

  if(munmap(mapped, pages * PGSIZE) < 0)
    fail("mmap page-state unmap");
  close(fd);
  if(unlink(path) < 0)
    fail("mmap test unlink");

  printf("memviztest: dynamic page states OK\n");
}

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

  uint64 covered = 0;
  for(int cell = 0; cell < MEMVIZ_CELLS; cell++){
    uint64 expected = cell_page_count(before.total_pages, cell);
    if(before.physical[cell].total_pages != expected)
      fail("cell coverage mismatch");
    covered += expected;
  }
  if(covered != before.total_pages)
    fail("cell coverage total mismatch");

  printf("memviztest: physical invariants OK\n");
}

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
  if(before.user_mirror_start != KUSERBASE)
    fail("user alias start");
  if(before.user_mirror_end != KUSERADDR(before.process_size))
    fail("user alias end");
  if(before.user_mirror_end > KUSEREND)
    fail("user alias exceeds window");

  printf("memviztest: kernel invariants OK\n");
}

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
  uint64 user_first_pa = 0;
  uint64 alias_first_pa = 0;

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

    if(entry->role == MEMVIZ_PTE_ROLE_ELF_FIRST && entry->present)
      user_first_pa = entry->pa;
    if(entry->role == MEMVIZ_PTE_ROLE_USER_STACK && entry->present)
      user_stack = 1;
    if(entry->role == MEMVIZ_PTE_ROLE_GUARD){
      if(entry->present && (entry->flags & PTE_U))
        fail("guard keeps PTE_U");
      guard_inaccessible = 1;
    }
    if(entry->role == MEMVIZ_PTE_ROLE_USER_MIRROR && entry->present){
      if(entry->va != KUSERBASE)
        fail("user alias pte VA");
      if(entry->flags & PTE_U)
        fail("user alias keeps PTE_U");
      alias_first_pa = entry->pa;
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
    fail("user alias pte missing");
  if(user_first_pa == 0 || alias_first_pa != user_first_pa)
    fail("user and alias PA mismatch");
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

static int
run_named(char *name)
{
  if(strcmp(name, "user") == 0){
    test_user_snapshot();
    test_user_render();
    test_dynamic_page_states();
  } else if(strcmp(name, "pid") == 0)
    test_target_pid_snapshot();
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

int
main(int argc, char **argv)
{
  if(argc == 1){
    test_user_snapshot();
    test_user_render();
    test_target_pid_snapshot();
    test_dynamic_page_states();
    test_physical_snapshot();
    test_allocate_and_release();
    test_kernel_snapshot();
    test_pagetable_snapshot();
  } else if(argc == 2){
    if(run_named(argv[1]) < 0){
      fprintf(2, "usage: memviztest [user|pid|phys|alloc|kernel|pagetable]\n");
      exit(1);
    }
  } else {
    fprintf(2, "usage: memviztest [user|pid|phys|alloc|kernel|pagetable]\n");
    exit(1);
  }

  printf("memviztest: OK\n");
  exit(0);
}
