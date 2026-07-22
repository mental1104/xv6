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
 * print_stack_bar 按低地址到高地址显示固定栈页。
 *
 * 左侧绿色点代表尚可向下增长的空间，右侧红色井号代表当前 SP 到栈顶
 * 已经占用的范围。
 */
static void
print_stack_bar(uint64 used, uint64 total, int plain)
{
  const int cells = 32;
  int used_cells = scaled_cells(used, total, cells);

  printf("stack   [");
  for(int i = 0; i < cells - used_cells; i++)
    print_glyph('.', ANSI_GREEN, plain);
  for(int i = 0; i < used_cells; i++)
    print_glyph('#', ANSI_RED, plain);
  printf("]  low <- grows down <- high\n");
}

/**
 * print_dynamic_bar 显示 dynamic_start 到当前用户增长上限的虚拟范围。
 */
static void
print_dynamic_bar(struct memviz_snapshot *s, int plain)
{
  uint64 extent = 0;
  uint64 capacity = 0;
  if(s->process_size > s->dynamic_start)
    extent = s->process_size - s->dynamic_start;
  if(s->user_limit > s->dynamic_start)
    capacity = s->user_limit - s->dynamic_start;

  int used_cells = scaled_cells(extent, capacity, MEMVIZ_CELLS);
  printf("dynamic [");
  for(int i = 0; i < used_cells; i++)
    print_glyph('#', ANSI_RED, plain);
  for(int i = used_cells; i < MEMVIZ_CELLS; i++)
    print_glyph('.', ANSI_GREEN, plain);
  printf("]\n");
}

/**
 * print_physical_bar 显示每段连续物理地址中的空闲页组成。
 */
static void
print_physical_bar(struct memviz_snapshot *s, int plain)
{
  printf("pages    [");
  for(int i = 0; i < MEMVIZ_CELLS; i++){
    struct memviz_cell *cell = &s->physical[i];
    if(cell->free_pages == 0)
      print_glyph('#', ANSI_RED, plain);
    else if(cell->free_pages == cell->total_pages)
      print_glyph('.', ANSI_GREEN, plain);
    else
      print_glyph(':', ANSI_YELLOW, plain);
  }
  printf("]\n");

  printf("legend   ");
  print_glyph('#', ANSI_RED, plain);
  printf(" allocated  ");
  print_glyph('.', ANSI_GREEN, plain);
  printf(" free  ");
  print_glyph(':', ANSI_YELLOW, plain);
  printf(" mixed\n");
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
  printf("\n%s=== CURRENT PROCESS USER ADDRESS SPACE ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);
  printf("high limit       %p  current xv6 growth limit\n", s->user_limit);
  printf("p->sz            %p  current low-address dynamic extent\n", s->process_size);
  printf("dynamic start    %p  first address above fixed user stack\n", s->dynamic_start);
  print_dynamic_bar(s, plain);

  if(s->user_stack_valid){
    uint64 stack_total = s->stack_top - s->stack_bottom;
    printf("stack top        %p\n", s->stack_top);
    printf("user sp          %p\n", s->user_sp);
    printf("stack bottom     %p\n", s->stack_bottom);
    printf("stack guard      %p - %p  not user accessible\n",
           s->stack_guard_start, s->stack_bottom);
    print_stack_bar(s->stack_used, stack_total, plain);
    printf("stack bytes      used=%d free=%d total=%d signal=%s\n",
           (int)s->stack_used, (int)s->stack_free, (int)stack_total,
           stack_signal(s));
  } else {
    printf("stack            invalid: current SP is not in a user stack page\n");
  }

  printf("ELF image        %p - %p\n", s->image_start, s->image_end);
  uint64 extent_pages = 0;
  uint64 remain_pages = 0;
  if(s->process_size > s->dynamic_start)
    extent_pages = (s->process_size - s->dynamic_start + PGSIZE - 1) / PGSIZE;
  if(s->user_limit > s->process_size)
    remain_pages = (s->user_limit - s->process_size) / PGSIZE;
  printf("virtual pages    dynamic=%d remaining-to-limit=%d\n",
         (int)extent_pages, (int)remain_pages);
  printf("physical pages   free=%d used=%d total=%d signal=%s\n",
         (int)s->free_pages, (int)s->used_pages, (int)s->total_pages,
         physical_signal(s));
  printf("note             p->sz may include sbrk and mmap ranges\n");
  printf("note             signals are current heuristics, not predictions\n");
}

/**
 * print_phys_view 输出 kalloc 管理范围和每 CPU freelist 统计。
 */
static void
print_phys_view(struct memviz_snapshot *s, int plain)
{
  printf("\n%s=== KALLOC PHYSICAL PAGE POOL ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);
  printf("managed range    %p - %p\n", s->kalloc_start, s->kalloc_end);
  print_physical_bar(s, plain);
  printf("pages            free=%d used=%d total=%d\n",
         (int)s->free_pages, (int)s->used_pages, (int)s->total_pages);
  printf("memory MiB       free=%d used=%d total=%d\n",
         (int)(s->free_pages * PGSIZE / (1024 * 1024)),
         (int)(s->used_pages * PGSIZE / (1024 * 1024)),
         (int)(s->total_pages * PGSIZE / (1024 * 1024)));
  printf("freelist pages   ");
  for(int i = 0; i < NCPU; i++){
    if(s->cpu_free_pages[i] != 0)
      printf("cpu%d=%d ", i, (int)s->cpu_free_pages[i]);
  }
  printf("\n");
  printf("OOM signal       %s\n", physical_signal(s));
}

/**
 * print_kernel_view 输出当前进程 p->kpagetable 的主要映射。
 */
static void
print_kernel_view(struct memviz_snapshot *s, int plain)
{
  printf("\n%s=== CURRENT PROCESS KERNEL ADDRESS SPACE ===%s\n",
         plain ? "" : ANSI_CYAN, plain ? "" : ANSI_RESET);
  printf("p->kpagetable    %p\n", s->kernel_pagetable);
  printf("TRAMPOLINE       %p - %p  RX\n",
         s->trampoline, s->trampoline + PGSIZE);

  if(s->kernel_stack_valid){
    uint64 total = s->kernel_stack_top - s->kernel_stack_bottom;
    printf("kernel stack top %p\n", s->kernel_stack_top);
    printf("kernel sp        %p\n", s->kernel_sp);
    printf("kernel stack     %p - %p\n",
           s->kernel_stack_bottom, s->kernel_stack_top);
    printf("kernel guard     %p - %p  unmapped\n",
           s->kernel_stack_guard_start, s->kernel_stack_bottom);
    print_stack_bar(s->kernel_stack_used, total, plain);
    printf("kernel stack B   used=%d free=%d total=%d\n",
           (int)s->kernel_stack_used, (int)s->kernel_stack_free, (int)total);
  } else {
    printf("kernel stack     invalid: current SP is outside p->kstack\n");
  }

  printf("RAM direct map   %p - %p\n",
         s->kernel_data_start, s->kernel_data_end);
  printf("kernel text RX   %p - %p\n",
         s->kernel_text_start, s->kernel_text_end);
  printf("VirtIO MMIO      %p - %p\n", s->virtio_start, s->virtio_end);
  printf("UART MMIO        %p - %p\n", s->uart_start, s->uart_end);
  printf("PLIC MMIO        %p - %p\n", s->plic_start, s->plic_end);
  printf("CLINT MMIO       %p - %p\n", s->clint_start, s->clint_end);
  printf("user mirror      %p - %p  supervisor-only aliases\n",
         s->user_mirror_start, s->user_mirror_end);
  printf("note             kernel stack usage includes this syscall path\n");
}

int
memviz_print(int view, int plain)
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
  default:
    return -1;
  }
  return 0;
}

int
memviz_print_all(int plain)
{
  if(memviz_print(MEMVIZ_VIEW_USER, plain) < 0)
    return -1;
  if(memviz_print(MEMVIZ_VIEW_PHYS, plain) < 0)
    return -1;
  if(memviz_print(MEMVIZ_VIEW_KERNEL, plain) < 0)
    return -1;
  return 0;
}
