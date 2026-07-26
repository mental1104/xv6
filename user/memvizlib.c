#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/riscv.h"
#include "kernel/memviz.h"
#include "user/user.h"
#include "user/memvizlib.h"

#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_CYAN "\033[36m"
#define ANSI_RESET "\033[0m"

#define BAR_CELLS 32
#define PHYS_COLS 16
#define PT_COLS 16

// 使用静态缓冲避免在固定一页的用户栈上放置较大的快照结构体。
static struct memviz_snapshot snapshot;

static void
print_glyph(char glyph, char *color, int plain)
{
  if(plain)
    printf("%c", glyph);
  else
    printf("%s%c%s", color, glyph, ANSI_RESET);
}

static int
string_length(char *text)
{
  int length = 0;
  while(text[length] != 0)
    length++;
  return length;
}

static int
string_contains(char *text, char *pattern)
{
  if(pattern == 0 || pattern[0] == 0)
    return 1;
  for(int i = 0; text[i] != 0; i++){
    int j = 0;
    while(pattern[j] != 0 && text[i + j] == pattern[j])
      j++;
    if(pattern[j] == 0)
      return 1;
  }
  return 0;
}

static void
print_decimal_padded(int value, int width)
{
  int divisor = 1;
  for(int i = 1; i < width; i++)
    divisor *= 10;
  while(divisor > 1 && value < divisor){
    printf("0");
    divisor /= 10;
  }
  printf("%d", value);
}

static char *
pte_role_name(int role)
{
  switch(role){
  case MEMVIZ_PTE_ROLE_ELF_FIRST:
    return "ELF first";
  case MEMVIZ_PTE_ROLE_ELF_LAST:
    return "ELF last";
  case MEMVIZ_PTE_ROLE_GUARD:
    return "guard";
  case MEMVIZ_PTE_ROLE_USER_STACK:
    return "user stack";
  case MEMVIZ_PTE_ROLE_DYNAMIC:
    return "dynamic";
  case MEMVIZ_PTE_ROLE_USER_MIRROR:
    return "user alias";
  case MEMVIZ_PTE_ROLE_KERNEL_STACK_GUARD:
    return "kstack guard";
  case MEMVIZ_PTE_ROLE_KERNEL_STACK:
    return "kernel stack";
  case MEMVIZ_PTE_ROLE_TRAMPOLINE:
    return "trampoline";
  case MEMVIZ_PTE_ROLE_KERNEL_TEXT:
    return "kernel text";
  case MEMVIZ_PTE_ROLE_RAM_DIRECT:
    return "RAM direct";
  case MEMVIZ_PTE_ROLE_UART:
    return "UART";
  case MEMVIZ_PTE_ROLE_VIRTIO:
    return "VirtIO";
  case MEMVIZ_PTE_ROLE_CLINT:
    return "CLINT";
  case MEMVIZ_PTE_ROLE_PLIC:
    return "PLIC";
  default:
    return "unknown";
  }
}

static char *
pte_space_name(int space)
{
  if(space == MEMVIZ_PTE_SPACE_USER)
    return "USER";
  if(space == MEMVIZ_PTE_SPACE_KERNEL)
    return "KERNEL";
  return "UNKNOWN";
}

static int
parse_filter_address(char *filter, uint64 *value)
{
  if(filter == 0 || filter[0] == 0)
    return 0;

  int base = 10;
  int index = 0;
  if(filter[0] == '0' && (filter[1] == 'x' || filter[1] == 'X')){
    base = 16;
    index = 2;
    if(filter[index] == 0)
      return 0;
  }

  uint64 result = 0;
  for(; filter[index] != 0; index++){
    int digit;
    if(filter[index] >= '0' && filter[index] <= '9')
      digit = filter[index] - '0';
    else if(base == 16 && filter[index] >= 'a' && filter[index] <= 'f')
      digit = filter[index] - 'a' + 10;
    else if(base == 16 && filter[index] >= 'A' && filter[index] <= 'F')
      digit = filter[index] - 'A' + 10;
    else
      return 0;
    if(digit >= base)
      return 0;
    result = result * base + digit;
  }

  *value = result;
  return 1;
}

static int
pte_entry_matches(struct memviz_pte_entry *entry, char *filter)
{
  if(filter == 0 || filter[0] == 0)
    return 1;

  uint64 address;
  if(parse_filter_address(filter, &address))
    return PGROUNDDOWN(address) == entry->va;
  if(string_contains(pte_role_name(entry->role), filter))
    return 1;
  if(strcmp(filter, "user") == 0 && entry->space == MEMVIZ_PTE_SPACE_USER)
    return 1;
  if(strcmp(filter, "kernel") == 0 && entry->space == MEMVIZ_PTE_SPACE_KERNEL)
    return 1;
  if(strcmp(filter, "mmio") == 0 &&
     (entry->role == MEMVIZ_PTE_ROLE_UART ||
      entry->role == MEMVIZ_PTE_ROLE_VIRTIO ||
      entry->role == MEMVIZ_PTE_ROLE_CLINT ||
      entry->role == MEMVIZ_PTE_ROLE_PLIC))
    return 1;
  return 0;
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

static int
kalloc_cell_index(struct memviz_snapshot *state, uint64 pa)
{
  if(state->total_pages == 0 || pa < state->kalloc_start || pa >= state->kalloc_end)
    return -1;
  uint64 page = (pa - state->kalloc_start) / PGSIZE;
  uint64 cell = page * MEMVIZ_CELLS / state->total_pages;
  if(cell >= MEMVIZ_CELLS)
    return -1;
  return (int)cell;
}

static char
kalloc_cell_state(struct memviz_snapshot *state, int cell)
{
  if(cell < 0 || cell >= MEMVIZ_CELLS)
    return '?';
  struct memviz_cell *value = &state->physical[cell];
  if(value->free_pages == 0)
    return '#';
  if(value->free_pages == value->total_pages)
    return '.';
  return ':';
}

static void
print_kalloc_cell(struct memviz_snapshot *state, uint64 pa, int plain)
{
  int cell = kalloc_cell_index(state, pa);
  if(cell < 0){
    printf("fixed/mmio");
    return;
  }

  char glyph = kalloc_cell_state(state, cell);
  printf("cell ");
  print_decimal_padded(cell, 2);
  printf(" ");
  if(glyph == '#')
    print_glyph(glyph, ANSI_RED, plain);
  else if(glyph == '.')
    print_glyph(glyph, ANSI_GREEN, plain);
  else
    print_glyph(glyph, ANSI_YELLOW, plain);
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
print_bar(int used_cells, char used, char free, char *used_color,
          char *free_color, int plain)
{
  printf("[");
  for(int i = 0; i < used_cells; i++)
    print_glyph(used, used_color, plain);
  for(int i = used_cells; i < BAR_CELLS; i++)
    print_glyph(free, free_color, plain);
  printf("]");
}

static void
print_reverse_bar(int used_cells, char used, char free, char *used_color,
                  char *free_color, int plain)
{
  printf("[");
  for(int i = 0; i < BAR_CELLS - used_cells; i++)
    print_glyph(free, free_color, plain);
  for(int i = 0; i < used_cells; i++)
    print_glyph(used, used_color, plain);
  printf("]");
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

static void
print_box_bar(int used_cells, char used, char free, char *used_color,
              char *free_color, int plain)
{
  printf("           | ");
  print_bar(used_cells, used, free, used_color, free_color, plain);
  printf(" |\n");
}

static void
print_box_reverse_bar(int used_cells, char used, char free, char *used_color,
                      char *free_color, int plain)
{
  printf("           | ");
  print_reverse_bar(used_cells, used, free, used_color, free_color, plain);
  printf(" |\n");
}

static uint64
dynamic_pages(struct memviz_snapshot *state)
{
  if(state->process_size <= state->dynamic_start)
    return 0;
  return (state->process_size - state->dynamic_start + PGSIZE - 1) / PGSIZE;
}

static uint64
dynamic_capacity_pages(struct memviz_snapshot *state)
{
  if(state->user_limit <= state->dynamic_start)
    return 0;
  return (state->user_limit - state->dynamic_start) / PGSIZE;
}

static uint64
remaining_pages(struct memviz_snapshot *state)
{
  if(state->user_limit <= state->process_size)
    return 0;
  return (state->user_limit - state->process_size) / PGSIZE;
}

static char *
stack_signal(struct memviz_snapshot *state)
{
  if(!state->user_stack_valid)
    return "invalid";
  if(state->stack_free <= 512)
    return "critical";
  if(state->stack_free <= 1024)
    return "low";
  return "normal";
}

static char *
physical_signal(struct memviz_snapshot *state)
{
  if(state->total_pages == 0)
    return "invalid";
  uint64 percent = state->free_pages * 100 / state->total_pages;
  if(percent <= 1)
    return "critical";
  if(percent <= 10)
    return "low";
  return "normal";
}

static void
print_process_pid(struct memviz_snapshot *state)
{
  printf("observed process pid=%d\n", state->process_pid);
}

static void
print_user_view(struct memviz_snapshot *state, int plain)
{
  uint64 used_pages = dynamic_pages(state);
  uint64 capacity_pages = dynamic_capacity_pages(state);
  int used_cells = scaled_cells(used_pages, capacity_pages, BAR_CELLS);

  printf("\n%s=== PROCESS USER VIRTUAL ADDRESS SPACE ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);
  print_process_pid(state);
  printf("\n       HIGH ADDRESS\n");
  print_line(state->user_limit, "+------------- USERMAX --------------+");
  print_box_text("AVAILABLE USER VA RANGE");
  print_box_bar(0, '#', '.', ANSI_RED, ANSI_GREEN, plain);
  printf("           | remaining: %d pages", (int)remaining_pages(state));
  for(int i = string_length("remaining:  pages") + 1; i < 34; i++)
    printf(" ");
  printf(" |\n");
  print_box_text("MMIO numbers are not the VA ceiling");
  print_line(state->process_size, "+-------------- p->sz ---------------+");
  print_box_text("DYNAMIC EXTENT");
  print_box_bar(used_cells, '#', '.', ANSI_RED, ANSI_GREEN, plain);
  printf("           | pages=%d", (int)used_pages);
  for(int i = 7; i < 34; i++)
    printf(" ");
  printf(" |\n");
  print_line(state->dynamic_start, "+---------- dynamic start -----------+");

  if(state->user_stack_valid){
    uint64 total = state->stack_top - state->stack_bottom;
    int stack_cells = scaled_cells(state->stack_used, total, BAR_CELLS);
    print_box_text("USER STACK");
    print_box_reverse_bar(stack_cells, '#', '.', ANSI_RED, ANSI_GREEN, plain);
    printf("           | used=%d B free=%d B",
           (int)state->stack_used, (int)state->stack_free);
    for(int i = 0; i < 14; i++)
      printf(" ");
    printf(" |\n");
  } else {
    print_box_text("USER STACK: invalid SP");
  }

  print_line(state->stack_bottom, "+------------------------------------+");
  print_box_text("GUARD PAGE / no PTE_U");
  print_box_bar(BAR_CELLS, 'X', 'X', ANSI_RED, ANSI_RED, plain);
  print_line(state->stack_guard_start, "+------------------------------------+");
  print_box_text("ELF IMAGE");
  print_box_bar(BAR_CELLS, '#', '#', ANSI_RED, ANSI_RED, plain);
  print_line(state->image_start, "+------------------------------------+");
  printf("       LOW ADDRESS\n");

  printf("\nsummary:\n");
  printf("  user limit: %p (USERMAX)\n", state->user_limit);
  printf("  stack: used=%d free=%d signal=%s\n",
         (int)state->stack_used, (int)state->stack_free, stack_signal(state));
  printf("  dynamic pages: %d\n", (int)used_pages);
  printf("  physical pages: free=%d used=%d total=%d signal=%s\n",
         (int)state->free_pages, (int)state->used_pages,
         (int)state->total_pages, physical_signal(state));
}

static void
print_phys_view(struct memviz_snapshot *state, int plain)
{
  printf("\n%s=== KALLOC PHYSICAL PAGE POOL ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);
  printf("\nPA %p\n", state->kalloc_start);
  printf("      ");
  for(int col = 0; col < PHYS_COLS; col++){
    print_decimal_padded(col, 2);
    printf(" ");
  }
  printf("\n");

  for(int row = 0; row * PHYS_COLS < MEMVIZ_CELLS; row++){
    int first = row * PHYS_COLS;
    print_decimal_padded(first, 3);
    printf("   ");
    for(int col = 0; col < PHYS_COLS && first + col < MEMVIZ_CELLS; col++){
      char glyph = kalloc_cell_state(state, first + col);
      if(glyph == '#')
        print_glyph(glyph, ANSI_RED, plain);
      else if(glyph == '.')
        print_glyph(glyph, ANSI_GREEN, plain);
      else
        print_glyph(glyph, ANSI_YELLOW, plain);
      printf("  ");
    }
    printf("\n");
  }
  printf("PA %p\n", state->kalloc_end);
  printf("\nlegend: # no free page, . all free, : mixed\n");
  printf("pages: free=%d used=%d total=%d\n",
         (int)state->free_pages, (int)state->used_pages, (int)state->total_pages);
  printf("freelist pages: ");
  for(int cpu = 0; cpu < NCPU; cpu++){
    if(state->cpu_free_pages[cpu] != 0)
      printf("cpu%d=%d ", cpu, (int)state->cpu_free_pages[cpu]);
  }
  printf("\nOOM signal: %s\n", physical_signal(state));
}

static void
print_kernel_view(struct memviz_snapshot *state, int plain)
{
  uint64 alias_limit = state->user_mirror_start + state->user_limit;
  uint64 lower_canonical_limit = state->trampoline + PGSIZE;

  printf("\n%s=== PROCESS KERNEL ADDRESS SPACE ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);
  print_process_pid(state);
  printf("\n       HIGH ADDRESS\n");
  print_line(alias_limit, "+------------ KUSEREND --------------+");
  print_box_text("unused alias capacity");
  print_line(state->user_mirror_end, "+------- current alias extent -------+");
  print_box_text("USER ALIAS WINDOW / supervisor-only");
  print_box_text("alias VA -> same user physical page");
  print_line(state->user_mirror_start, "+---------- KUSERBASE ---------------+");
  print_box_text("SV39 NON-CANONICAL ADDRESS GAP");
  print_box_text("walk() rejects this entire interval");
  print_line(lower_canonical_limit, "+-------------- MAXVA ---------------+");
  print_box_text("TRAMPOLINE / RX");
  print_line(state->trampoline, "+------------------------------------+");

  if(state->kernel_stack_valid){
    uint64 total = state->kernel_stack_top - state->kernel_stack_bottom;
    int stack_cells = scaled_cells(state->kernel_stack_used, total, BAR_CELLS);
    print_box_text("KERNEL STACK");
    print_box_reverse_bar(stack_cells, '#', '.', ANSI_RED, ANSI_GREEN, plain);
  } else {
    print_box_text("KERNEL STACK: invalid SP");
  }
  print_line(state->kernel_stack_bottom, "+------------------------------------+");
  print_box_text("KERNEL STACK GUARD");
  print_box_bar(BAR_CELLS, 'X', 'X', ANSI_RED, ANSI_RED, plain);
  print_line(state->kernel_stack_guard_start, "+------------------------------------+");
  print_box_text("RAM DIRECT MAP");
  print_line(state->kernel_data_start, "+------------------------------------+");
  print_box_text("KERNEL TEXT / RX");
  print_line(state->kernel_text_start, "+---------- KERNBASE ----------------+");
  print_box_text("PLIC / CLINT / UART / VIRTIO");
  print_box_text("low-address MMIO fixed mappings");
  printf("       LOW ADDRESS\n");

  printf("\nsummary:\n");
  printf("  p->kpagetable: %p\n", state->kernel_pagetable);
  printf("  user alias window: %p - %p\n",
         state->user_mirror_start, alias_limit);
  printf("  current alias extent: %p - %p\n",
         state->user_mirror_start, state->user_mirror_end);
  printf("  non-canonical gap: %p - %p\n",
         lower_canonical_limit, state->user_mirror_start);
  printf("  RAM direct map: %p - %p\n",
         state->kernel_data_start, state->kernel_data_end);
  printf("  MMIO: uart=%p virtio=%p clint=%p plic=%p\n",
         state->uart_start, state->virtio_start,
         state->clint_start, state->plic_start);
}

static void
print_pagetable_level(struct memviz_pte_level *level, int last)
{
  printf("      %s L%d[%d] ", last ? "`--" : "|--",
         level->level, level->index);
  if(!level->present){
    printf("empty\n");
    return;
  }
  printf("pte=%p flags=", level->pte);
  print_pte_flags(level->flags);
  printf(" -> %p\n", level->pa);
}

static void
print_pagetable_tree(struct memviz_snapshot *state,
                     struct memviz_pte_entry *entry, int plain)
{
  printf("  %s  VA=%p\n", pte_role_name(entry->role), entry->va);
  for(int index = 0; index < 3; index++){
    struct memviz_pte_level *level = &entry->levels[index];
    print_pagetable_level(level, index == 2 || !level->present);
    if(!level->present)
      return;
  }

  if(!entry->present){
    printf("      `-- no leaf mapping\n");
    return;
  }
  printf("      `-- PA=%p kalloc=", entry->pa);
  print_kalloc_cell(state, entry->pa, plain);
  printf("\n");
  if(entry->role == MEMVIZ_PTE_ROLE_GUARD && (entry->flags & PTE_U) == 0)
    printf("          note: guard has no PTE_U\n");
  if(entry->role == MEMVIZ_PTE_ROLE_USER_MIRROR && (entry->flags & PTE_U) == 0)
    printf("          note: same PA as user page, supervisor-only alias\n");
}

static int
print_pagetable_forest(struct memviz_snapshot *state, int space, char *title,
                       uint64 root, int plain, char *filter)
{
  int printed = 0;
  printf("\n%s root=%p\n", title, root);
  for(int index = 0; index < (int)state->pagetable_entry_count; index++){
    struct memviz_pte_entry *entry = &state->pagetable_entries[index];
    if(entry->space != space || !pte_entry_matches(entry, filter))
      continue;
    print_pagetable_tree(state, entry, plain);
    printed++;
  }
  if(printed == 0)
    printf("  (no matching occupied path)\n");
  return printed;
}

static void
print_pt_usage_page(struct memviz_pt_usage_page *page, int plain)
{
  printf("\n%s L%d pagetable PA %p\n",
         pte_space_name(page->space), page->level, page->pa);
  printf("entries: used=%d free=%d total=%d\n",
         (int)page->used_entries,
         (int)(page->total_entries - page->used_entries),
         (int)page->total_entries);
  printf("      ");
  for(int col = 0; col < PT_COLS; col++){
    print_decimal_padded(col, 2);
    printf(" ");
  }
  printf("\n");

  for(int row = 0; row * PT_COLS < MEMVIZ_PT_USAGE_CELLS; row++){
    int first = row * PT_COLS;
    print_decimal_padded(first, 3);
    printf("   ");
    for(int col = 0; col < PT_COLS && first + col < MEMVIZ_PT_USAGE_CELLS; col++){
      struct memviz_pt_usage_cell *cell = &page->cells[first + col];
      if(cell->used_entries == 0)
        print_glyph('.', ANSI_GREEN, plain);
      else if(cell->used_entries == cell->total_entries)
        print_glyph('#', ANSI_RED, plain);
      else
        print_glyph(':', ANSI_YELLOW, plain);
      printf("  ");
    }
    printf("\n");
  }
}

static void
print_pagetable_usage_view(struct memviz_snapshot *state, int plain)
{
  printf("\n%s=== PAGE TABLE SLOT USAGE ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);
  print_process_pid(state);
  printf("\nEach matrix compresses 512 PTE slots into 64 cells.\n");
  for(int index = 0; index < (int)state->pagetable_usage_count; index++)
    print_pt_usage_page(&state->pagetable_usage[index], plain);
  printf("\nlegend: . all empty, # all occupied, : mixed\n");
  printf("pages shown: %d limit=%d\n",
         (int)state->pagetable_usage_count, MEMVIZ_PT_USAGE_PAGES);
}

static void
print_pagetable_view(struct memviz_snapshot *state, int plain, char *filter)
{
  if(filter != 0 && strcmp(filter, "usage") == 0){
    print_pagetable_usage_view(state, plain);
    return;
  }

  printf("\n%s=== PROCESS PAGE TABLE TREE ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);
  print_process_pid(state);
  printf("\nflow: root -> L2 -> L1 -> L0 leaf -> PA -> kalloc cell\n");
  if(filter != 0)
    printf("filter: %s\n", filter);

  int total = 0;
  total += print_pagetable_forest(state, MEMVIZ_PTE_SPACE_USER,
                                  "USER PAGETABLE", state->user_pagetable,
                                  plain, filter);
  total += print_pagetable_forest(state, MEMVIZ_PTE_SPACE_KERNEL,
                                  "KERNEL PAGETABLE", state->kernel_pagetable,
                                  plain, filter);
  printf("\nlegend: flags V/R/W/X/U/C; alias has no U bit\n");
  printf("filter examples: guard, stack, alias, user, kernel, mmio, 0x5000, usage\n");
  printf("printed paths: %d\n", total);
}

/** 采集当前进程或指定 PID 的快照到静态用户态缓冲。 */
static int
load_snapshot(int pid, int view)
{
  if(pid > 0)
    return memsnapshot_pid(pid, view, &snapshot);
  return memsnapshot(view, &snapshot);
}

int
memviz_print_pid_filtered(int pid, int view, int plain, char *filter)
{
  if(pid <= 0 || load_snapshot(pid, view) < 0){
    fprintf(2, "memviz: cannot snapshot pid %d for view %d\n", pid, view);
    return -1;
  }

  switch(view){
  case MEMVIZ_VIEW_USER:
    print_user_view(&snapshot, plain);
    break;
  case MEMVIZ_VIEW_PHYS:
    print_phys_view(&snapshot, plain);
    break;
  case MEMVIZ_VIEW_KERNEL:
    print_kernel_view(&snapshot, plain);
    break;
  case MEMVIZ_VIEW_PAGETABLE:
    print_pagetable_view(&snapshot, plain, filter);
    break;
  default:
    return -1;
  }
  return 0;
}

int
memviz_print_filtered(int view, int plain, char *filter)
{
  if(load_snapshot(0, view) < 0){
    fprintf(2, "memviz: memsnapshot failed for view %d\n", view);
    return -1;
  }

  switch(view){
  case MEMVIZ_VIEW_USER:
    print_user_view(&snapshot, plain);
    break;
  case MEMVIZ_VIEW_PHYS:
    print_phys_view(&snapshot, plain);
    break;
  case MEMVIZ_VIEW_KERNEL:
    print_kernel_view(&snapshot, plain);
    break;
  case MEMVIZ_VIEW_PAGETABLE:
    print_pagetable_view(&snapshot, plain, filter);
    break;
  default:
    return -1;
  }
  return 0;
}

int
memviz_print(int view, int plain)
{
  return memviz_print_filtered(view, plain, 0);
}

int
memviz_print_pid(int pid, int view, int plain)
{
  return memviz_print_pid_filtered(pid, view, plain, 0);
}

int
memviz_print_all(int plain)
{
  if(memviz_print(MEMVIZ_VIEW_USER, plain) < 0)
    return -1;
  if(memviz_print(MEMVIZ_VIEW_PAGETABLE, plain) < 0)
    return -1;
  if(memviz_print(MEMVIZ_VIEW_PHYS, plain) < 0)
    return -1;
  if(memviz_print(MEMVIZ_VIEW_KERNEL, plain) < 0)
    return -1;
  return 0;
}

int
memviz_print_all_pid(int pid, int plain)
{
  if(memviz_print_pid(pid, MEMVIZ_VIEW_USER, plain) < 0)
    return -1;
  if(memviz_print_pid(pid, MEMVIZ_VIEW_PAGETABLE, plain) < 0)
    return -1;
  if(memviz_print_pid(pid, MEMVIZ_VIEW_PHYS, plain) < 0)
    return -1;
  if(memviz_print_pid(pid, MEMVIZ_VIEW_KERNEL, plain) < 0)
    return -1;
  return 0;
}
