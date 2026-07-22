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

// 使用静态缓冲避免在固定一页的用户栈上放置较大的快照结构体。
static struct memviz_snapshot snapshot;

/**
 * print_glyph 输出一个可选 ANSI 颜色的字符。
 *
 * @param glyph 要显示的单字节字符。
 * @param color ANSI 前景色；plain 非零时忽略。
 * @param plain 非零表示纯文本模式。
 */
static void
print_glyph(char glyph, char *color, int plain)
{
  if(plain)
    printf("%c", glyph);
  else
    printf("%s%c%s", color, glyph, ANSI_RESET);
}

/**
 * string_length 计算以 NUL 结尾字符串的显示长度。
 *
 * @param s 非空的静态字符串；函数不会修改其内容。
 * @return 不包含结尾 NUL 的字节数。
 */
static int
string_length(char *s)
{
  int n = 0;
  while(s[n] != 0)
    n++;
  return n;
}

/**
 * print_decimal_padded 输出左侧补零的十进制整数。
 *
 * @param value 需要输出的非负整数。
 * @param width 最小显示宽度；实际位数超过该值时不截断。
 */
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

/**
 * pte_role_name 返回页表观察角色的固定显示名。
 *
 * @param role MEMVIZ_PTE_ROLE_* 之一。
 * @return 角色对应的静态字符串；未知值返回 unknown。
 */
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
    return "user mirror";
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

/**
 * pte_space_name 返回页表所属地址空间的显示名。
 *
 * @param space MEMVIZ_PTE_SPACE_* 之一。
 * @return 地址空间对应的静态字符串。
 */
static char *
pte_space_name(int space)
{
  if(space == MEMVIZ_PTE_SPACE_USER)
    return "USER";
  if(space == MEMVIZ_PTE_SPACE_KERNEL)
    return "KERNEL";
  return "UNKNOWN";
}

/**
 * string_contains 判断文本中是否包含指定子串。
 *
 * @param text 被搜索的静态文本。
 * @param pattern 用户传入的过滤片段；空串视为匹配。
 * @return 包含时返回 1，否则返回 0。
 */
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

/**
 * parse_filter_address 将十进制或 0x 前缀地址解析为 VA。
 *
 * @param filter 用户命令行过滤参数。
 * @param value 解析成功时写入地址值。
 * @return filter 完全是数字或 0x 十六进制时返回 1；否则返回 0。
 */
static int
parse_filter_address(char *filter, uint64 *value)
{
  if(filter == 0 || filter[0] == 0)
    return 0;

  int base = 10;
  int i = 0;
  if(filter[0] == '0' && (filter[1] == 'x' || filter[1] == 'X')){
    base = 16;
    i = 2;
    if(filter[i] == 0)
      return 0;
  }

  uint64 result = 0;
  for(; filter[i] != 0; i++){
    int digit;
    if(filter[i] >= '0' && filter[i] <= '9')
      digit = filter[i] - '0';
    else if(base == 16 && filter[i] >= 'a' && filter[i] <= 'f')
      digit = filter[i] - 'a' + 10;
    else if(base == 16 && filter[i] >= 'A' && filter[i] <= 'F')
      digit = filter[i] - 'A' + 10;
    else
      return 0;
    if(digit >= base)
      return 0;
    result = result * base + digit;
  }

  *value = result;
  return 1;
}

/**
 * pte_entry_matches 判断页表链路是否满足用户过滤条件。
 *
 * @param entry 待检查的页表观察条目。
 * @param filter 用户传入的 role 片段或 VA；为空时全部匹配。
 * @return 满足过滤条件时返回 1，否则返回 0。
 */
static int
pte_entry_matches(struct memviz_pte_entry *entry, char *filter)
{
  if(filter == 0 || filter[0] == 0)
    return 1;

  uint64 va;
  if(parse_filter_address(filter, &va))
    return PGROUNDDOWN(va) == entry->va;

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

/**
 * is_pagetable_usage_filter 判断用户是否请求页表槽位余量视图。
 *
 * @param filter 用户传入的 pagetable 过滤参数。
 * @return filter 等于 usage 时返回 1，否则返回 0。
 */
static int
is_pagetable_usage_filter(char *filter)
{
  return filter != 0 && strcmp(filter, "usage") == 0;
}

/**
 * print_pte_flags 输出 xv6/RISC-V PTE 权限位。
 *
 * @param flags PTE_FLAGS(pte) 得到的低位权限集合。
 */
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

/**
 * kalloc_cell_index 将物理地址映射到物理池压缩 cell。
 *
 * @param s 已包含 kalloc 物理池快照的 memviz_snapshot。
 * @param pa 待定位的物理地址。
 * @return 落在 kalloc 管理范围内时返回 cell 下标；否则返回 -1。
 */
static int
kalloc_cell_index(struct memviz_snapshot *s, uint64 pa)
{
  if(s->total_pages == 0 || pa < s->kalloc_start || pa >= s->kalloc_end)
    return -1;
  uint64 page = (pa - s->kalloc_start) / PGSIZE;
  uint64 cell = (page * MEMVIZ_CELLS) / s->total_pages;
  if(cell >= MEMVIZ_CELLS)
    return -1;
  return (int)cell;
}

/**
 * kalloc_cell_state 返回压缩物理 cell 的空闲状态字符。
 *
 * @param s 已包含 kalloc 物理池快照的 memviz_snapshot。
 * @param cell kalloc_cell_index 返回的 cell 下标。
 * @return '#', '.', ':' 或 '?'；含义与 memviz phys 的 legend 一致。
 */
static char
kalloc_cell_state(struct memviz_snapshot *s, int cell)
{
  if(cell < 0 || cell >= MEMVIZ_CELLS)
    return '?';
  struct memviz_cell *c = &s->physical[cell];
  if(c->free_pages == 0)
    return '#';
  if(c->free_pages == c->total_pages)
    return '.';
  return ':';
}

/**
 * print_kalloc_cell 输出页表 PA 对应的物理池位置。
 *
 * @param s 已包含 kalloc 物理池快照的 memviz_snapshot。
 * @param pa 页表叶子 PTE 指向的物理页地址。
 * @param plain 非零时禁用 ANSI 颜色。
 */
static void
print_kalloc_cell(struct memviz_snapshot *s, uint64 pa, int plain)
{
  int cell = kalloc_cell_index(s, pa);
  if(cell < 0){
    printf("fixed/mmio");
    return;
  }

  char state = kalloc_cell_state(s, cell);
  printf("cell ");
  print_decimal_padded(cell, 2);
  printf(" ");
  if(state == '#')
    print_glyph(state, ANSI_RED, plain);
  else if(state == '.')
    print_glyph(state, ANSI_GREEN, plain);
  else
    print_glyph(state, ANSI_YELLOW, plain);
}

/**
 * scaled_cells 将某段用量压缩为固定宽度字符数。
 *
 * @param value 已使用的字节数或页数。
 * @param total 对应总量，必须与 value 使用相同单位。
 * @param cells 字符条总宽度。
 * @return 向上取整后的占用字符数，范围为 0 到 cells。
 */
static int
scaled_cells(uint64 value, uint64 total, int cells)
{
  if(total == 0 || value == 0)
    return 0;
  uint64 result = (value * cells + total - 1) / total;
  if(result > (uint64)cells)
    result = cells;
  return result;
}

/**
 * print_bar 输出固定宽度条形图。
 *
 * @param used_cells 左侧 used 字符数量，调用方已限制在 0 到 BAR_CELLS。
 * @param used 表示已使用部分的字符。
 * @param free 表示剩余部分的字符。
 * @param used_color used 字符在 ANSI 模式下使用的颜色。
 * @param free_color free 字符在 ANSI 模式下使用的颜色。
 * @param plain 非零时禁用 ANSI 颜色。
 */
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

/**
 * print_reverse_bar 输出低地址在左、高地址在右的栈用量。
 *
 * @param used_cells 右侧 used 字符数量，调用方已限制在 0 到 BAR_CELLS。
 * @param used 表示已使用部分的字符。
 * @param free 表示剩余部分的字符。
 * @param used_color used 字符在 ANSI 模式下使用的颜色。
 * @param free_color free 字符在 ANSI 模式下使用的颜色。
 * @param plain 非零时禁用 ANSI 颜色。
 */
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

/**
 * print_line 输出带地址标签的竖向边界线。
 *
 * @param address 当前边界对应的真实虚拟地址。
 * @param mark 固定宽度边界线文本。
 * @param label 额外标签；允许为空字符串。
 */
static void
print_line(uint64 address, char *mark, char *label)
{
  printf("%p %s %s\n", address, mark, label);
}

/**
 * print_box_text 输出竖向柱形图中的文本行。
 *
 * @param text 需要放入柱形图内部的静态文本。
 */
static void
print_box_text(char *text)
{
  int n = string_length(text);
  printf("           | %s", text);
  for(int i = n; i < 34; i++)
    printf(" ");
  printf(" |\n");
}

/**
 * print_box_bar 输出竖向柱形图中的条形图行。
 *
 * @param used_cells 左侧 used 字符数量。
 * @param used 表示已使用部分的字符。
 * @param free 表示剩余部分的字符。
 * @param used_color used 字符在 ANSI 模式下使用的颜色。
 * @param free_color free 字符在 ANSI 模式下使用的颜色。
 * @param plain 非零时禁用 ANSI 颜色。
 */
static void
print_box_bar(int used_cells, char used, char free, char *used_color,
              char *free_color, int plain)
{
  printf("           | ");
  print_bar(used_cells, used, free, used_color, free_color, plain);
  printf(" |\n");
}

/**
 * print_box_reverse_bar 输出栈这类高地址在右侧的条形图行。
 *
 * @param used_cells 右侧 used 字符数量。
 * @param used 表示已使用部分的字符。
 * @param free 表示剩余部分的字符。
 * @param used_color used 字符在 ANSI 模式下使用的颜色。
 * @param free_color free 字符在 ANSI 模式下使用的颜色。
 * @param plain 非零时禁用 ANSI 颜色。
 */
static void
print_box_reverse_bar(int used_cells, char used, char free,
                      char *used_color, char *free_color, int plain)
{
  printf("           | ");
  print_reverse_bar(used_cells, used, free, used_color, free_color, plain);
  printf(" |\n");
}

/**
 * dynamic_pages 计算 dynamic_start 到 p->sz 的页数。
 *
 * @param s 当前进程快照；process_size 小于等于 dynamic_start 时视为 0。
 * @return 向上取整后的 dynamic extent 页数。
 */
static uint64
dynamic_pages(struct memviz_snapshot *s)
{
  if(s->process_size <= s->dynamic_start)
    return 0;
  return (s->process_size - s->dynamic_start + PGSIZE - 1) / PGSIZE;
}

/**
 * remaining_pages 计算 p->sz 到用户上限之间的剩余页数。
 *
 * @param s 当前进程快照；user_limit 小于等于 process_size 时返回 0。
 * @return 可继续增长的整页数量。
 */
static uint64
remaining_pages(struct memviz_snapshot *s)
{
  if(s->user_limit <= s->process_size)
    return 0;
  return (s->user_limit - s->process_size) / PGSIZE;
}

/**
 * dynamic_capacity_pages 计算 dynamic_start 到用户上限的容量。
 *
 * @param s 当前进程快照；user_limit 小于等于 dynamic_start 时返回 0。
 * @return dynamic extent 可增长范围的整页数量。
 */
static uint64
dynamic_capacity_pages(struct memviz_snapshot *s)
{
  if(s->user_limit <= s->dynamic_start)
    return 0;
  return (s->user_limit - s->dynamic_start) / PGSIZE;
}

/**
 * print_pages_line 输出不超过柱形图宽度的页数摘要。
 */
static void
print_pages_line(char *prefix, uint64 pages)
{
  printf("           | %s%d pages", prefix, (int)pages);
  int n = string_length(prefix) + string_length(" pages");
  int digits = 1;
  uint64 value = pages;
  while(value >= 10){
    digits++;
    value /= 10;
  }
  n += digits;
  for(int i = n; i < 34; i++)
    printf(" ");
  printf(" |\n");
}

/**
 * print_bytes_line 输出栈用量摘要，并保持柱形图右边界对齐。
 */
static void
print_bytes_line(char *prefix, uint64 used, uint64 free)
{
  printf("           | %s%d B / free %d B", prefix, (int)used, (int)free);
  int n = string_length(prefix) + string_length(" B / free  B");
  uint64 value = used;
  int digits = 1;
  while(value >= 10){
    digits++;
    value /= 10;
  }
  n += digits;
  value = free;
  digits = 1;
  while(value >= 10){
    digits++;
    value /= 10;
  }
  n += digits;
  for(int i = n; i < 34; i++)
    printf(" ");
  printf(" |\n");
}

/**
 * stack_signal 将剩余栈空间转换为教学用风险信号。
 */
static char *
stack_signal(struct memviz_snapshot *s)
{
  if(!s->user_stack_valid)
    return "invalid";
  if(s->stack_free <= 512)
    return "critical";
  if(s->stack_free <= 1024)
    return "low";
  return "normal";
}

/**
 * physical_signal 根据可立即分配页的比例返回教学用 OOM 风险信号。
 */
static char *
physical_signal(struct memviz_snapshot *s)
{
  if(s->total_pages == 0)
    return "invalid";
  uint64 percent = s->free_pages * 100 / s->total_pages;
  if(percent <= 1)
    return "critical";
  if(percent <= 10)
    return "low";
  return "normal";
}

/**
 * print_user_view 输出当前进程的低地址用户布局和风险余量。
 */
static void
print_user_view(struct memviz_snapshot *s, int plain)
{
  printf("\n%s=== CURRENT PROCESS USER VIRTUAL ADDRESS SPACE ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);

  uint64 dyn_pages = dynamic_pages(s);
  uint64 dyn_capacity = dynamic_capacity_pages(s);
  uint64 remain = remaining_pages(s);
  int dyn_cells = scaled_cells(dyn_pages, dyn_capacity, BAR_CELLS);

  printf("\n       HIGH ADDRESS\n");
  print_line(s->user_limit, "+------------------------------------+", "");
  print_box_text("AVAILABLE VIRTUAL ADDRESS RANGE");
  print_box_bar(0, '#', '.', ANSI_RED, ANSI_GREEN, plain);
  print_pages_line("remaining: ", remain);
  print_box_text("current ceiling: PLIC");
  print_line(s->process_size, "+-------------- p->sz ---------------+", "");
  print_box_text("DYNAMIC EXTENT");
  if(dyn_pages == 0){
    print_box_text("empty");
  } else {
    print_box_bar(dyn_cells, '#', '.', ANSI_RED, ANSI_GREEN, plain);
    print_pages_line("pages=", dyn_pages);
    print_box_text("grows upward from dynamic start");
  }
  print_line(s->dynamic_start, "+---------- dynamic start -----------+", "");

  if(s->user_stack_valid){
    uint64 stack_total = s->stack_top - s->stack_bottom;
    int stack_cells = scaled_cells(s->stack_used, stack_total, BAR_CELLS);
    print_box_text("USER STACK");
    print_box_reverse_bar(stack_cells, '#', '.', ANSI_RED, ANSI_GREEN, plain);
    print_bytes_line("used ", s->stack_used, s->stack_free);
    print_box_text("grows downward from stack top");
  } else {
    print_box_text("USER STACK");
    print_box_text("invalid: SP outside user stack");
  }

  print_line(s->stack_bottom, "+------------------------------------+", "");
  print_box_text("GUARD PAGE");
  print_box_bar(BAR_CELLS, 'X', 'X', ANSI_RED, ANSI_RED, plain);
  print_box_text("not user accessible");
  print_line(s->stack_guard_start, "+------------------------------------+", "");
  print_box_text("ELF IMAGE");
  print_box_bar(BAR_CELLS, '#', '#', ANSI_RED, ANSI_RED, plain);
  print_line(s->image_start, "+------------------------------------+", "");
  printf("       LOW ADDRESS\n");

  printf("\nsummary:\n");
  if(s->user_stack_valid){
    uint64 stack_total = s->stack_top - s->stack_bottom;
    printf("  stack: used=%d free=%d total=%d signal=%s\n",
           (int)s->stack_used, (int)s->stack_free, (int)stack_total,
           stack_signal(s));
  } else {
    printf("  stack: invalid\n");
  }
  printf("  dynamic pages: %d\n", (int)dyn_pages);
  printf("  physical pages: free=%d used=%d total=%d signal=%s\n",
         (int)s->free_pages, (int)s->used_pages, (int)s->total_pages,
         physical_signal(s));
}

/**
 * print_phys_view 输出 kalloc 管理范围和每 CPU freelist 统计。
 */
static void
print_phys_view(struct memviz_snapshot *s, int plain)
{
  printf("\n%s=== KALLOC PHYSICAL PAGE POOL ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);

  printf("\nPA %p\n", s->kalloc_start);
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
      struct memviz_cell *cell = &s->physical[first + col];
      if(cell->free_pages == 0)
        print_glyph('#', ANSI_RED, plain);
      else if(cell->free_pages == cell->total_pages)
        print_glyph('.', ANSI_GREEN, plain);
      else
        print_glyph(':', ANSI_YELLOW, plain);
      printf("  ");
    }
    printf("\n");
  }
  printf("PA %p\n", s->kalloc_end);

  printf("\nlegend:\n");
  printf("  ");
  print_glyph('#', ANSI_RED, plain);
  printf(" no free page in compressed cell\n");
  printf("  ");
  print_glyph('.', ANSI_GREEN, plain);
  printf(" all pages free\n");
  printf("  ");
  print_glyph(':', ANSI_YELLOW, plain);
  printf(" mixed free/used pages\n");

  printf("\npages: free=%d used=%d total=%d\n",
         (int)s->free_pages, (int)s->used_pages, (int)s->total_pages);
  printf("memory MiB: free=%d used=%d total=%d\n",
         (int)(s->free_pages * PGSIZE / (1024 * 1024)),
         (int)(s->used_pages * PGSIZE / (1024 * 1024)),
         (int)(s->total_pages * PGSIZE / (1024 * 1024)));
  printf("freelist pages: ");
  for(int i = 0; i < NCPU; i++){
    if(s->cpu_free_pages[i] != 0)
      printf("cpu%d=%d ", i, (int)s->cpu_free_pages[i]);
  }
  printf("\n");
  printf("OOM signal: %s\n", physical_signal(s));
}

/**
 * print_kernel_view 输出当前进程 p->kpagetable 的主要映射。
 */
static void
print_kernel_view(struct memviz_snapshot *s, int plain)
{
  printf("\n%s=== CURRENT PROCESS KERNEL ADDRESS SPACE ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);

  printf("\n       HIGH ADDRESS\n");
  print_line(s->trampoline + PGSIZE, "+------------------------------------+", "");
  print_box_text("TRAMPOLINE");
  print_box_text("fixed mapping / RX");
  print_line(s->trampoline, "+------------------------------------+", "");
  if(s->kernel_stack_valid){
    uint64 total = s->kernel_stack_top - s->kernel_stack_bottom;
    int stack_cells = scaled_cells(s->kernel_stack_used, total, BAR_CELLS);
    print_box_text("KERNEL STACK");
    print_box_reverse_bar(stack_cells, '#', '.', ANSI_RED, ANSI_GREEN, plain);
    print_box_text("current kernel SP");
  } else {
    print_box_text("KERNEL STACK");
    print_box_text("invalid: SP outside p->kstack");
  }
  print_line(s->kernel_stack_bottom, "+------------------------------------+", "");
  print_box_text("KERNEL STACK GUARD");
  print_box_bar(BAR_CELLS, 'X', 'X', ANSI_RED, ANSI_RED, plain);
  print_line(s->kernel_stack_guard_start, "+------------------------------------+", "");
  print_box_text("RAM DIRECT MAP");
  print_box_text("kernel data through PHYSTOP");
  print_line(s->kernel_data_start, "+------------------------------------+", "");
  print_box_text("KERNEL TEXT");
  print_box_text("fixed mapping / RX");
  print_line(s->kernel_text_start, "+------------------------------------+", "");
  print_box_text("PLIC / CLINT / UART / VIRTIO");
  print_box_text("MMIO fixed mappings");
  print_line(s->user_mirror_end, "+------------------------------------+", "");
  print_box_text("USER MIRROR, supervisor-only");
  print_box_text("same physical user pages");
  print_box_text("not a second copy");
  print_line(s->user_mirror_start, "+------------------------------------+", "");
  printf("       LOW ADDRESS\n");

  printf("\nsummary:\n");
  printf("  p->kpagetable: %p\n", s->kernel_pagetable);
  if(s->kernel_stack_valid){
    uint64 total = s->kernel_stack_top - s->kernel_stack_bottom;
    printf("  kernel stack: used=%d free=%d total=%d\n",
           (int)s->kernel_stack_used, (int)s->kernel_stack_free, (int)total);
  } else {
    printf("  kernel stack: invalid\n");
  }
  printf("  RAM direct map: %p - %p\n",
         s->kernel_data_start, s->kernel_data_end);
  printf("  MMIO: uart=%p virtio=%p clint=%p plic=%p\n",
         s->uart_start, s->virtio_start, s->clint_start, s->plic_start);
}

/**
 * print_pagetable_level 输出页表树中的一层 PTE。
 *
 * @param level 已采集的一层页表 PTE 记录。
 * @param last 非零表示这是当前链路的最后一层，用于选择树形连接符。
 */
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

/**
 * print_pagetable_tree 输出一条 VA 到 leaf PTE 的三级树状链路。
 *
 * @param s 已采集完成的快照，包含页表条目和 kalloc cell 状态。
 * @param entry 需要打印的页表观察条目。
 * @param plain 非零时禁用 ANSI 颜色。
 */
static void
print_pagetable_tree(struct memviz_snapshot *s, struct memviz_pte_entry *entry,
                     int plain)
{
  printf("  %s  VA=%p\n", pte_role_name(entry->role), entry->va);
  for(int i = 0; i < 3; i++){
    struct memviz_pte_level *level = &entry->levels[i];
    print_pagetable_level(level, i == 2 || !level->present);
    if(!level->present)
      return;
  }

  if(!entry->present){
    printf("      `-- no leaf mapping\n");
    return;
  }

  printf("      `-- PA=%p kalloc=", entry->pa);
  print_kalloc_cell(s, entry->pa, plain);
  printf("\n");
  if(entry->role == MEMVIZ_PTE_ROLE_GUARD && (entry->flags & PTE_U) == 0)
    printf("          note: guard has no PTE_U; user mode cannot access it\n");
  if(entry->role == MEMVIZ_PTE_ROLE_USER_MIRROR && (entry->flags & PTE_U) == 0)
    printf("          note: same PA as user page, supervisor-only mirror\n");
}

/**
 * print_pagetable_forest 输出用户页表或内核页表的一组链路树。
 *
 * @param s 已采集完成的快照。
 * @param space MEMVIZ_PTE_SPACE_USER 或 MEMVIZ_PTE_SPACE_KERNEL。
 * @param title 分区标题。
 * @param root 页表根物理地址，仅用于说明当前查询对象。
 * @param plain 非零时禁用 ANSI 颜色。
 * @param filter 用户传入的 role 片段或 VA；为空时打印全部。
 * @return 实际打印出的链路数量。
 */
static int
print_pagetable_forest(struct memviz_snapshot *s, int space, char *title,
                       uint64 root, int plain, char *filter)
{
  int printed = 0;
  printf("\n%s root=%p\n", title, root);
  for(int i = 0; i < (int)s->pagetable_entry_count; i++){
    struct memviz_pte_entry *entry = &s->pagetable_entries[i];
    if(entry->space != space || !pte_entry_matches(entry, filter))
      continue;
    print_pagetable_tree(s, entry, plain);
    printed++;
  }
  if(printed == 0)
    printf("  (no matching occupied path)\n");
  return printed;
}

/**
 * print_pt_usage_cell 输出一个 PTE 槽位压缩 cell 的占用状态。
 *
 * @param cell 当前压缩 cell，包含总槽数和 valid PTE 数。
 * @param plain 非零时禁用 ANSI 颜色。
 */
static void
print_pt_usage_cell(struct memviz_pt_usage_cell *cell, int plain)
{
  if(cell->used_entries == 0)
    print_glyph('.', ANSI_GREEN, plain);
  else if(cell->used_entries == cell->total_entries)
    print_glyph('#', ANSI_RED, plain);
  else
    print_glyph(':', ANSI_YELLOW, plain);
}

/**
 * print_pt_usage_page 输出一个页表页的 PTE 槽位矩阵。
 *
 * @param page 已采集的页表页占用摘要。
 * @param plain 非零时禁用 ANSI 颜色。
 */
static void
print_pt_usage_page(struct memviz_pt_usage_page *page, int plain)
{
  const int cols = 16;
  uint64 free_entries = page->total_entries - page->used_entries;

  printf("\n%s L%d pagetable PA %p\n",
         pte_space_name(page->space), page->level, page->pa);
  printf("entries: used=%d free=%d total=%d\n",
         (int)page->used_entries, (int)free_entries,
         (int)page->total_entries);
  printf("      ");
  for(int col = 0; col < cols; col++){
    print_decimal_padded(col, 2);
    printf(" ");
  }
  printf("\n");

  for(int row = 0; row * cols < MEMVIZ_PT_USAGE_CELLS; row++){
    int first = row * cols;
    print_decimal_padded(first, 3);
    printf("   ");
    for(int col = 0; col < cols && first + col < MEMVIZ_PT_USAGE_CELLS; col++){
      print_pt_usage_cell(&page->cells[first + col], plain);
      printf("  ");
    }
    printf("\n");
  }
}

/**
 * print_pagetable_usage_view 输出已分配页表页的槽位余量矩阵。
 *
 * @param s 已采集完成的快照，包含用户和内核页表页摘要。
 * @param plain 非零时禁用 ANSI 颜色。
 */
static void
print_pagetable_usage_view(struct memviz_snapshot *s, int plain)
{
  printf("\n%s=== PAGE TABLE SLOT USAGE ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);
  printf("\nEach matrix compresses 512 PTE slots into 64 cells.\n");

  for(int i = 0; i < (int)s->pagetable_usage_count; i++)
    print_pt_usage_page(&s->pagetable_usage[i], plain);

  printf("\nlegend:\n");
  printf("  ");
  print_glyph('.', ANSI_GREEN, plain);
  printf(" all PTE slots empty in compressed cell\n");
  printf("  ");
  print_glyph('#', ANSI_RED, plain);
  printf(" all PTE slots occupied in compressed cell\n");
  printf("  ");
  print_glyph(':', ANSI_YELLOW, plain);
  printf(" mixed empty/occupied PTE slots\n");
  printf("pages shown: %d limit=%d\n",
         (int)s->pagetable_usage_count, MEMVIZ_PT_USAGE_PAGES);
}

/**
 * print_pagetable_view 输出当前进程页表到物理页池的树状闭环观察。
 *
 * @param s 已采集完成的快照。
 * @param plain 非零时禁用 ANSI 颜色。
 * @param filter 用户传入的 role 片段或 VA；为空时打印全部。
 */
static void
print_pagetable_view(struct memviz_snapshot *s, int plain, char *filter)
{
  if(is_pagetable_usage_filter(filter)){
    print_pagetable_usage_view(s, plain);
    return;
  }

  printf("\n%s=== CURRENT PROCESS PAGE TABLE TREE ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);
  printf("\nflow: root page table -> L2 -> L1 -> L0 leaf -> PA -> kalloc cell\n");
  if(filter != 0)
    printf("filter: %s\n", filter);

  int total = 0;
  total += print_pagetable_forest(s, MEMVIZ_PTE_SPACE_USER,
                                  "USER PAGETABLE", s->user_pagetable,
                                  plain, filter);
  total += print_pagetable_forest(s, MEMVIZ_PTE_SPACE_KERNEL,
                                  "KERNEL PAGETABLE", s->kernel_pagetable,
                                  plain, filter);

  printf("\nlegend:\n");
  printf("  L2/L1/L0 are Sv39 page-table indexes for the selected VA\n");
  printf("  flags: V valid, R read, W write, X exec, U user, C cow\n");
  printf("  kalloc cell: ");
  print_glyph('#', ANSI_RED, plain);
  printf(" no free page, ");
  print_glyph('.', ANSI_GREEN, plain);
  printf(" all free, ");
  print_glyph(':', ANSI_YELLOW, plain);
  printf(" mixed\n");
  printf("  filter examples: guard, stack, user, kernel, mmio, 0x5000, usage\n");
  printf("  printed paths: %d\n", total);
}

int
memviz_print_filtered(int view, int plain, char *filter)
{
  if(memsnapshot(view, &snapshot) < 0){
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
