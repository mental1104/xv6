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

#define USER_BAR_CELLS 32
#define USER_GAP_BREAK_MIN_PAGES 16

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

/**
 * usage 输出 memviz 支持的视图和纯文本选项。
 */
static void
usage(void)
{
  fprintf(2, "usage: memviz <user|phys|kernel|pagetable|all> [filter] [--plain]\n");
}

/**
 * string_length 返回以 NUL 结尾字符串的字节数。
 *
 * @param text 非空字符串。
 * @return 不包含结尾 NUL 的字节数。
 */
static int
string_length(char *text)
{
  int length = 0;
  while(text[length] != 0)
    length++;
  return length;
}

/**
 * print_glyph 输出一个可选 ANSI 颜色的单字节字符。
 *
 * @param glyph 要输出的字符。
 * @param color ANSI 颜色前缀。
 * @param plain 非零时禁止输出 ANSI 转义序列。
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
 * print_line 输出带真实地址的字符图边界线。
 *
 * @param address 边界虚拟地址。
 * @param mark 边界标签与横线。
 */
static void
print_line(uint64 address, char *mark)
{
  printf("%p %s\n", address, mark);
}

/**
 * print_box_text 输出地址空间框内的一行文本并补齐右边界。
 *
 * @param text 最长 34 字节的静态标签。
 */
static void
print_box_text(char *text)
{
  int length = string_length(text);
  printf("           | %s", text);
  for(int i = length; i < 34; i++)
    printf(" ");
  printf(" |\n");
}

/**
 * scaled_cells 将用量压缩为固定宽度字符数，非零用量至少占一个字符。
 *
 * @param used 已使用字节或页数。
 * @param total 总字节或总页数。
 * @param cells 条形图字符宽度。
 * @return 范围为 [0, cells] 的已使用字符数。
 */
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

/**
 * print_box_bar 输出低偏移到高偏移方向的页内用量条。
 *
 * @param used_cells 已使用字符数。
 * @param used 已使用区域字符。
 * @param free 未使用区域字符。
 * @param plain 非零时禁用 ANSI 颜色。
 */
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

/**
 * print_box_reverse_bar 输出高地址侧已使用的向下增长栈用量条。
 *
 * @param used_cells 已使用字符数。
 * @param plain 非零时禁用 ANSI 颜色。
 */
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

/**
 * print_pte_flags 输出教学相关的 RISC-V PTE 权限位。
 *
 * @param flags PTE 低十位 flags。
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
 * dynamic_pages 计算动态起点到 p->sz 已覆盖的页数。
 *
 * @param state 当前内存快照。
 * @return 动态区域向上取整后的页数。
 */
static uint64
dynamic_pages(struct memviz_snapshot *state)
{
  if(state->process_size <= state->dynamic_start)
    return 0;
  return (state->process_size - state->dynamic_start + PGSIZE - 1) / PGSIZE;
}

/**
 * remaining_pages 计算 p->sz 到 USERMAX 之间仍未使用的整页数。
 *
 * @param state 当前内存快照。
 * @return 普通用户 VA 的剩余整页数。
 */
static uint64
remaining_pages(struct memviz_snapshot *state)
{
  if(state->user_limit <= state->process_size)
    return 0;
  return (state->user_limit - state->process_size) / PGSIZE;
}

/**
 * print_user_gap 用明显的断裂线表现 p->sz 到 USERMAX 的巨大地址跨度。
 *
 * @param state 当前内存快照。
 *
 * 小于阈值时保留紧凑布局；超过阈值时增加纵向留白、断裂符号和
 * not-drawn-to-scale 提示，避免字符图暗示上下边界物理相邻。
 */
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

/**
 * print_trampoline_details 展示 trampoline 页的 VA、PA、权限和代码顺序。
 *
 * @param state 当前内存快照。
 */
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

/**
 * print_trapframe_details 按真实 ABI 顺序展开 trapframe 页内全部成员。
 *
 * @param state 当前内存快照。
 *
 * 每个槽位同时展示页内 offset、对应 VA、对应 PA 和 memsnapshot 系统调用
 * 入口时保存的值；该值不是任意时刻的实时寄存器。
 */
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

/**
 * print_user_view 输出包含顶端固定页、巨大 VA 空洞和低地址进程区域的完整视图。
 *
 * @param plain 非零时禁用 ANSI 颜色。
 * @return 采样成功返回 0，系统调用失败返回 -1。
 */
static int
print_user_view(int plain)
{
  if(memsnapshot(MEMVIZ_VIEW_USER, &user_snapshot) < 0)
    return -1;

  uint64 used_pages = dynamic_pages(&user_snapshot);
  int dynamic_cells = scaled_cells(used_pages,
                                   remaining_pages(&user_snapshot) + used_pages,
                                   USER_BAR_CELLS);
  int trampoline_cells = scaled_cells(user_snapshot.trampoline_used,
                                      PGSIZE, USER_BAR_CELLS);
  int trapframe_cells = scaled_cells(user_snapshot.trapframe_used,
                                     PGSIZE, USER_BAR_CELLS);

  printf("\n%s=== CURRENT PROCESS USER VIRTUAL ADDRESS SPACE ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);
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
  print_box_text("DYNAMIC EXTENT");
  print_box_bar(dynamic_cells, '#', '.', plain);
  printf("           | pages=%d\n", (int)used_pages);
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
  printf("  dynamic pages: %d\n", (int)used_pages);
  printf("  physical pages: free=%d used=%d total=%d\n",
         (int)user_snapshot.free_pages, (int)user_snapshot.used_pages,
         (int)user_snapshot.total_pages);

  print_trampoline_details(&user_snapshot);
  print_trapframe_details(&user_snapshot);
  return 0;
}

/**
 * print_all_views 依次输出增强用户视图和其余已有视图。
 *
 * @param plain 非零时禁用 ANSI 颜色。
 * @return 全部视图成功返回 0；任一采样失败返回 -1。
 */
static int
print_all_views(int plain)
{
  if(print_user_view(plain) < 0)
    return -1;
  if(memviz_print(MEMVIZ_VIEW_PHYS, plain) < 0)
    return -1;
  if(memviz_print(MEMVIZ_VIEW_KERNEL, plain) < 0)
    return -1;
  return memviz_print(MEMVIZ_VIEW_PAGETABLE, plain);
}

/**
 * main 解析命令行并打印指定的当前进程内存视图。
 *
 * @param argc 参数数量。
 * @param argv 参数数组；第一个位置参数必须是视图名。
 * @return 成功返回 0；参数或采样失败返回 1。
 */
int
main(int argc, char **argv)
{
  if(argc < 2 || argc > 4){
    usage();
    exit(1);
  }

  int plain = 0;
  char *filter = 0;
  for(int i = 2; i < argc; i++){
    if(strcmp(argv[i], "--plain") == 0){
      plain = 1;
    } else if(filter == 0){
      filter = argv[i];
    } else {
      usage();
      exit(1);
    }
  }

  int result;
  if(strcmp(argv[1], "pagetable") != 0 && filter != 0){
    usage();
    exit(1);
  }

  if(strcmp(argv[1], "user") == 0)
    result = print_user_view(plain);
  else if(strcmp(argv[1], "phys") == 0)
    result = memviz_print(MEMVIZ_VIEW_PHYS, plain);
  else if(strcmp(argv[1], "kernel") == 0)
    result = memviz_print(MEMVIZ_VIEW_KERNEL, plain);
  else if(strcmp(argv[1], "pagetable") == 0)
    result = memviz_print_filtered(MEMVIZ_VIEW_PAGETABLE, plain, filter);
  else if(strcmp(argv[1], "all") == 0)
    result = print_all_views(plain);
  else {
    usage();
    exit(1);
  }

  exit(result == 0 ? 0 : 1);
}
