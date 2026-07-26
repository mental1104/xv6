#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/riscv.h"
#include "kernel/memviz.h"
#include "user/user.h"
#include "user/memvizlib.h"

#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BLUE "\033[34m"
#define ANSI_CYAN "\033[36m"
#define ANSI_ORANGE "\033[38;5;208m"
#define ANSI_RESET "\033[0m"

#define USER_BAR_CELLS 32
#define USER_GAP_BREAK_MIN_PAGES 16

_Static_assert(USER_BAR_CELLS == MEMVIZ_USER_STATE_CELLS,
               "user renderer width must match snapshot cells");

// 使用静态缓冲，避免扩展后的快照占满固定一页用户栈。
static struct memviz_snapshot user_snapshot;

// 名称顺序与 enum memviz_trapframe_slot 以及 struct trapframe ABI 一致。
static char *trapframe_slot_names[MEMVIZ_TRAPFRAME_SLOT_COUNT] = {
  "kernel_satp", "kernel_sp", "kernel_trap", "epc", "kernel_hartid",
  "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1",
  "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
  "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11",
  "t3", "t4", "t5", "t6",
};

enum dynamic_display_kind {
  DYNAMIC_RESIDENT = 1,
  DYNAMIC_LAZY = 2,
  DYNAMIC_MMAP = 3,
  DYNAMIC_COW = 4,
};

/** 输出 memviz 支持的视图、PID 注入和纯文本选项。 */
static void
usage(void)
{
  fprintf(2,
          "usage: memviz <user|phys|kernel|pagetable|all> [filter] [--pid pid] [--plain]\n");
}

static int
string_length(char *text)
{
  int length = 0;
  while(text[length] != 0)
    length++;
  return length;
}

static void
print_glyph(char glyph, char *color, int plain)
{
  if(plain)
    printf("%c", glyph);
  else
    printf("%s%c%s", color, glyph, ANSI_RESET);
}

static void
print_line(uint64 address, char *mark)
{
  printf("%p %s\n", address, mark);
}

static void
print_box_text(char *text)
{
  int length = string_length(text);
  printf("           | %s", text);
  for(int i = length; i < 34; i++)
    printf(" ");
  printf(" |\n");
}

static int
scaled_cells(uint64 used, uint64 total, int cells)
{
  if(total == 0 || used == 0)
    return 0;
  uint64 result = (used * cells + total - 1) / total;
  if(result > (uint64)cells)
    result = cells;
  return (int)result;
}

static void
print_box_bar(int used_cells, char used, char free, int plain)
{
  printf("           | [");
  for(int i = 0; i < USER_BAR_CELLS; i++){
    if(i < used_cells)
      print_glyph(used, ANSI_RED, plain);
    else
      print_glyph(free, ANSI_GREEN, plain);
  }
  printf("] |\n");
}

static void
print_box_reverse_bar(int used_cells, int plain)
{
  printf("           | [");
  for(int i = 0; i < USER_BAR_CELLS; i++){
    if(i < USER_BAR_CELLS - used_cells)
      print_glyph('.', ANSI_GREEN, plain);
    else
      print_glyph('#', ANSI_RED, plain);
  }
  printf("] |\n");
}

static void
print_pte_flags(uint64 flags)
{
  printf("%c%c%c%c%c%c",
         (flags & PTE_V) ? 'V' : '-',
         (flags & PTE_R) ? 'R' : '-',
         (flags & PTE_W) ? 'W' : '-',
         (flags & PTE_X) ? 'X' : '-',
         (flags & PTE_U) ? 'U' : '-',
         (flags & PTE_COW) ? 'C' : '-');
}

static uint64
remaining_pages(struct memviz_snapshot *state)
{
  if(state->user_limit <= state->process_size)
    return 0;
  return (state->user_limit - state->process_size) / PGSIZE;
}

static void
print_user_gap(struct memviz_snapshot *state)
{
  uint64 pages = remaining_pages(state);
  uint64 bytes = state->user_limit > state->process_size ?
                 state->user_limit - state->process_size : 0;

  print_box_text("AVAILABLE ORDINARY USER VA");
  printf("           | remaining=%d pages bytes=%p\n", (int)pages, bytes);
  print_box_text("currently unmapped above p->sz");

  if(pages >= USER_GAP_BREAK_MIN_PAGES){
    printf("           |                                    |\n");
    printf("           :                                    :\n");
    printf("           +====== ADDRESS-SPACE BREAK =========+\n");
    printf("           :        not drawn to scale           :\n");
    printf("           :                                    :\n");
    printf("           |                                    |\n");
  }
}

static int
dynamic_cell_kind(struct memviz_user_state_cell *cell)
{
  int kind = DYNAMIC_RESIDENT;
  uint best = cell->resident_pages;
  if(cell->lazy_pages >= best){
    kind = DYNAMIC_LAZY;
    best = cell->lazy_pages;
  }
  if(cell->mmap_pages >= best){
    kind = DYNAMIC_MMAP;
    best = cell->mmap_pages;
  }
  if(cell->cow_pages >= best)
    kind = DYNAMIC_COW;
  return kind;
}

static void
print_dynamic_state_bar(struct memviz_snapshot *state, int plain)
{
  printf("           | [");
  for(int index = 0; index < USER_BAR_CELLS; index++){
    if(index >= (int)state->dynamic_state_cell_count){
      printf(" ");
      continue;
    }

    int kind = dynamic_cell_kind(&state->dynamic_state[index]);
    if(kind == DYNAMIC_COW)
      print_glyph(plain ? 'C' : '.', ANSI_YELLOW, plain);
    else if(kind == DYNAMIC_LAZY)
      print_glyph(plain ? 'L' : '.', ANSI_BLUE, plain);
    else if(kind == DYNAMIC_MMAP)
      print_glyph(plain ? 'M' : '.', ANSI_ORANGE, plain);
    else
      print_glyph('#', ANSI_RED, plain);
  }
  printf("] |\n");
}

static void
print_dynamic_legend(int plain)
{
  printf("           | legend: ");
  print_glyph('#', ANSI_RED, plain);
  printf(" resident ");
  print_glyph(plain ? 'C' : '.', ANSI_YELLOW, plain);
  printf(" COW ");
  print_glyph(plain ? 'L' : '.', ANSI_BLUE, plain);
  printf(" lazy ");
  print_glyph(plain ? 'M' : '.', ANSI_ORANGE, plain);
  printf(" mmap\n");
}

static void
print_trampoline_details(struct memviz_snapshot *state)
{
  printf("\n=== TRAMPOLINE PAGE DETAIL ===\n");
  printf("  VA page: %p - %p\n", state->trampoline, state->maxva);
  printf("  PA page: %p - %p\n", state->trampoline_pa,
         state->trampoline_pa + PGSIZE);
  printf("  PTE flags: ");
  print_pte_flags(state->trampoline_flags);
  printf("  user-access=no\n");
  printf("  code used=%d B free=%d B\n",
         (int)state->trampoline_used,
         (int)(PGSIZE - state->trampoline_used));
  printf("  logical order, low offset -> high offset:\n");

  if(state->uservec_offset > 0){
    printf("    [0, %d) alignment/prefix  PA=%p-%p\n",
           (int)state->uservec_offset,
           state->trampoline_pa,
           state->trampoline_pa + state->uservec_offset);
  }
  printf("    [%d, %d) uservec  VA=%p PA=%p\n",
         (int)state->uservec_offset, (int)state->userret_offset,
         state->trampoline + state->uservec_offset,
         state->trampoline_pa + state->uservec_offset);
  printf("    [%d, %d) userret  VA=%p PA=%p\n",
         (int)state->userret_offset, (int)state->trampoline_used,
         state->trampoline + state->userret_offset,
         state->trampoline_pa + state->userret_offset);
  printf("    [%d, %d) unused by trampoline code  PA=%p-%p\n",
         (int)state->trampoline_used, PGSIZE,
         state->trampoline_pa + state->trampoline_used,
         state->trampoline_pa + PGSIZE);
}

static void
print_trapframe_details(struct memviz_snapshot *state)
{
  printf("\n=== TRAPFRAME PAGE MEMBER ORDER ===\n");
  printf("  VA page: %p - %p\n", state->trapframe, state->trampoline);
  printf("  PA page: %p - %p\n", state->trapframe_pa,
         state->trapframe_pa + PGSIZE);
  printf("  PTE flags: ");
  print_pte_flags(state->trapframe_flags);
  printf("  user-access=no\n");
  printf("  struct used=%d B free-tail=%d B\n",
         (int)state->trapframe_used,
         (int)(PGSIZE - state->trapframe_used));
  printf("  values are captured inside memsnapshot before syscall a0 is replaced\n");

  for(int slot = 0; slot < MEMVIZ_TRAPFRAME_SLOT_COUNT; slot++){
    if(slot == MEMVIZ_TF_KERNEL_SATP)
      printf("  -- kernel entry/return context --\n");
    if(slot == MEMVIZ_TF_RA)
      printf("  -- saved user registers in ABI storage order --\n");

    uint64 offset = (uint64)slot * sizeof(uint64);
    printf("  [%d] offset=%d name=%s VA=%p PA=%p value=%p\n",
           slot, (int)offset, trapframe_slot_names[slot],
           state->trapframe + offset,
           state->trapframe_pa + offset,
           state->trapframe_values[slot]);
  }

  printf("  unused tail: offset=%d..%d VA=%p PA=%p bytes=%d\n",
         (int)state->trapframe_used, PGSIZE,
         state->trapframe + state->trapframe_used,
         state->trapframe_pa + state->trapframe_used,
         (int)(PGSIZE - state->trapframe_used));
}

/** 采集当前进程或显式 PID，并输出完整用户地址空间视图。 */
static int
print_user_view(int target_pid, int plain)
{
  int result;
  if(target_pid > 0)
    result = memsnapshot_pid(target_pid, MEMVIZ_VIEW_USER, &user_snapshot);
  else
    result = memsnapshot(MEMVIZ_VIEW_USER, &user_snapshot);
  if(result < 0){
    if(target_pid > 0)
      fprintf(2, "memviz: cannot snapshot pid %d\n", target_pid);
    return -1;
  }

  int trampoline_cells = scaled_cells(user_snapshot.trampoline_used,
                                      PGSIZE, USER_BAR_CELLS);
  int trapframe_cells = scaled_cells(user_snapshot.trapframe_used,
                                     PGSIZE, USER_BAR_CELLS);

  printf("\n%s=== PROCESS USER VIRTUAL ADDRESS SPACE ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);
  printf("observed process pid=%d\n", user_snapshot.process_pid);
  printf("\n       HIGH ADDRESS\n");
  print_line(user_snapshot.maxva, "+--------------- MAXVA ---------------+");
  print_box_text("TRAMPOLINE / supervisor-only RX");
  print_box_bar(trampoline_cells, '#', '.', plain);
  print_box_text("one physical page; no PTE_U");
  print_line(user_snapshot.trampoline, "+------------ TRAMPOLINE -------------+");
  print_box_text("TRAPFRAME / supervisor-only RW");
  print_box_bar(trapframe_cells, '#', '.', plain);
  print_box_text("one physical page; no PTE_U");
  print_line(user_snapshot.trapframe, "+------ USERMAX / TRAPFRAME -----------+");

  print_user_gap(&user_snapshot);
  print_line(user_snapshot.process_size, "+--------------- p->sz ----------------+");
  print_box_text("DYNAMIC EXTENT / page states");
  print_dynamic_state_bar(&user_snapshot, plain);
  printf("           | pages=%d resident=%d cow=%d\n",
         (int)user_snapshot.dynamic_page_count,
         (int)user_snapshot.dynamic_resident_pages,
         (int)user_snapshot.dynamic_cow_pages);
  printf("           | lazy=%d mmap=%d cells=%d\n",
         (int)user_snapshot.dynamic_lazy_pages,
         (int)user_snapshot.dynamic_mmap_pages,
         (int)user_snapshot.dynamic_state_cell_count);
  print_dynamic_legend(plain);
  print_line(user_snapshot.dynamic_start, "+----------- dynamic start ------------+");

  if(user_snapshot.user_stack_valid){
    uint64 stack_total = user_snapshot.stack_top - user_snapshot.stack_bottom;
    int stack_cells = scaled_cells(user_snapshot.stack_used,
                                   stack_total, USER_BAR_CELLS);
    print_box_text("USER STACK / grows downward");
    print_box_reverse_bar(stack_cells, plain);
    printf("           | used=%d B free=%d B\n",
           (int)user_snapshot.stack_used, (int)user_snapshot.stack_free);
  } else {
    print_box_text("USER STACK: invalid SP");
  }

  print_line(user_snapshot.stack_bottom, "+--------------------------------------+");
  print_box_text("GUARD PAGE / mapped without PTE_U");
  print_box_bar(USER_BAR_CELLS, 'X', 'X', plain);
  print_line(user_snapshot.stack_guard_start, "+--------------------------------------+");
  print_box_text("ELF IMAGE");
  print_box_bar(USER_BAR_CELLS, '#', '#', plain);
  print_line(user_snapshot.image_start, "+--------------------------------------+");
  printf("       LOW ADDRESS\n");

  printf("\nsummary:\n");
  printf("  ordinary user limit: %p (USERMAX/TRAPFRAME)\n",
         user_snapshot.user_limit);
  printf("  architectural low-half limit: %p (MAXVA)\n", user_snapshot.maxva);
  printf("  trampoline: VA=%p PA=%p used=%d/%d flags=",
         user_snapshot.trampoline, user_snapshot.trampoline_pa,
         (int)user_snapshot.trampoline_used, PGSIZE);
  print_pte_flags(user_snapshot.trampoline_flags);
  printf("\n");
  printf("  trapframe: VA=%p PA=%p used=%d/%d flags=",
         user_snapshot.trapframe, user_snapshot.trapframe_pa,
         (int)user_snapshot.trapframe_used, PGSIZE);
  print_pte_flags(user_snapshot.trapframe_flags);
  printf("\n");
  printf("  p->sz to USERMAX: pages=%d bytes=%p\n",
         (int)remaining_pages(&user_snapshot),
         user_snapshot.user_limit - user_snapshot.process_size);
  printf("  stack: used=%d free=%d\n",
         (int)user_snapshot.stack_used, (int)user_snapshot.stack_free);
  printf("  dynamic: pages=%d resident=%d cow=%d lazy=%d mmap=%d\n",
         (int)user_snapshot.dynamic_page_count,
         (int)user_snapshot.dynamic_resident_pages,
         (int)user_snapshot.dynamic_cow_pages,
         (int)user_snapshot.dynamic_lazy_pages,
         (int)user_snapshot.dynamic_mmap_pages);
  printf("  physical pages: free=%d used=%d total=%d\n",
         (int)user_snapshot.free_pages, (int)user_snapshot.used_pages,
         (int)user_snapshot.total_pages);

  print_trampoline_details(&user_snapshot);
  print_trapframe_details(&user_snapshot);
  return 0;
}

static int
print_all_views(int target_pid, int plain)
{
  if(print_user_view(target_pid, plain) < 0)
    return -1;
  if(target_pid > 0){
    if(memviz_print_pid(target_pid, MEMVIZ_VIEW_PHYS, plain) < 0)
      return -1;
    if(memviz_print_pid(target_pid, MEMVIZ_VIEW_KERNEL, plain) < 0)
      return -1;
    return memviz_print_pid(target_pid, MEMVIZ_VIEW_PAGETABLE, plain);
  }
  if(memviz_print(MEMVIZ_VIEW_PHYS, plain) < 0)
    return -1;
  if(memviz_print(MEMVIZ_VIEW_KERNEL, plain) < 0)
    return -1;
  return memviz_print(MEMVIZ_VIEW_PAGETABLE, plain);
}

int
main(int argc, char **argv)
{
  if(argc < 2 || argc > 6){
    usage();
    exit(1);
  }

  int plain = 0;
  int target_pid = 0;
  char *filter = 0;
  for(int i = 2; i < argc; i++){
    if(strcmp(argv[i], "--plain") == 0){
      if(plain){
        usage();
        exit(1);
      }
      plain = 1;
    } else if(strcmp(argv[i], "--pid") == 0){
      if(target_pid != 0 || i + 1 >= argc){
        usage();
        exit(1);
      }
      target_pid = atoi(argv[++i]);
      if(target_pid <= 0){
        usage();
        exit(1);
      }
    } else if(filter == 0){
      filter = argv[i];
    } else {
      usage();
      exit(1);
    }
  }

  if(strcmp(argv[1], "pagetable") != 0 && filter != 0){
    usage();
    exit(1);
  }

  int result;
  if(strcmp(argv[1], "user") == 0)
    result = print_user_view(target_pid, plain);
  else if(strcmp(argv[1], "phys") == 0)
    result = target_pid > 0 ?
      memviz_print_pid(target_pid, MEMVIZ_VIEW_PHYS, plain) :
      memviz_print(MEMVIZ_VIEW_PHYS, plain);
  else if(strcmp(argv[1], "kernel") == 0)
    result = target_pid > 0 ?
      memviz_print_pid(target_pid, MEMVIZ_VIEW_KERNEL, plain) :
      memviz_print(MEMVIZ_VIEW_KERNEL, plain);
  else if(strcmp(argv[1], "pagetable") == 0)
    result = target_pid > 0 ?
      memviz_print_pid_filtered(target_pid, MEMVIZ_VIEW_PAGETABLE, plain, filter) :
      memviz_print_filtered(MEMVIZ_VIEW_PAGETABLE, plain, filter);
  else if(strcmp(argv[1], "all") == 0)
    result = print_all_views(target_pid, plain);
  else {
    usage();
    exit(1);
  }

  exit(result == 0 ? 0 : 1);
}
