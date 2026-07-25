#ifndef XV6_MEMVIZTEST_REGION_MAPPING_H
#define XV6_MEMVIZTEST_REGION_MAPPING_H

#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/memviz.h"
#include "user/user.h"

static struct memviz_snapshot region_before;
static struct memviz_snapshot region_deep_stack;
static struct memviz_snapshot region_lazy;
static struct memviz_snapshot region_resident;
static struct memviz_snapshot region_restored;
static volatile int region_stack_sink;

/** 输出分段概念映射实验的失败原因并以非零状态终止。 */
static void
region_mapping_fail(char *message)
{
  printf("regionmapping: FAIL: %s\n", message);
  exit(1);
}

/**
 * 查询当前进程一个用户虚拟地址的 Sv39 叶子映射。
 *
 * @param va 待观察的用户虚拟地址，允许当前尚未建立叶子 PTE。
 * @param query 接收三级页表路径和最终叶子状态。
 */
static void
region_mapping_query(uint64 va, struct memviz_va_query *query)
{
  if(vaquery(va, query) < 0)
    region_mapping_fail("vaquery rejected ordinary user address");
}

/** 输出一个代表页的教学相关 PTE 权限位。 */
static void
region_mapping_print_flags(uint64 flags)
{
  printf("%c%c%c%c%c%c",
         (flags & PTE_V) ? 'V' : '-',
         (flags & PTE_R) ? 'R' : '-',
         (flags & PTE_W) ? 'W' : '-',
         (flags & PTE_X) ? 'X' : '-',
         (flags & PTE_U) ? 'U' : '-',
         (flags & PTE_COW) ? 'C' : '-');
}

static void region_mapping_capture_deep_stack(int depth) __attribute__((noinline));

/**
 * 在更深的真实用户调用栈中采集快照，用 SP 下降证明栈按低地址方向使用。
 *
 * @param depth 还需建立的递归栈帧数量；每层保留一块 volatile 数据防止尾调用消除。
 */
static void
region_mapping_capture_deep_stack(int depth)
{
  volatile char frame[96];
  frame[0] = (char)depth;
  frame[sizeof(frame) - 1] = (char)(depth + 1);

  if(depth == 0){
    if(memsnapshot(MEMVIZ_VIEW_USER, &region_deep_stack) < 0)
      region_mapping_fail("deep stack snapshot");
  } else {
    region_mapping_capture_deep_stack(depth - 1);
  }

  // 递归返回后仍读取当前帧，防止编译器把递归改写成尾调用。
  region_stack_sink += frame[0] + frame[sizeof(frame) - 1];
}

/**
 * 验证教材分段概念在当前 xv6 中只能映射为逻辑区域和分页证据。
 *
 * 实验不创建段描述符，也不声称 RISC-V Sv39 实现了 base/bounds 分段。它检查 ELF、
 * guard、栈和动态区域的真实边界与 PTE，并通过递归和 sbrk 分别观察向下使用的栈与
 * 向高地址增长的动态范围。
 */
static void
memviztest_region_mapping(void)
{
  struct memviz_va_query image;
  struct memviz_va_query guard;
  struct memviz_va_query stack;
  struct memviz_va_query dynamic;

  if(memsnapshot(MEMVIZ_VIEW_USER, &region_before) < 0)
    region_mapping_fail("baseline snapshot");
  if(!region_before.user_stack_valid)
    region_mapping_fail("baseline user stack");

  if(region_before.image_start >= region_before.image_end)
    region_mapping_fail("ELF image range");
  if(region_before.image_end != region_before.stack_guard_start)
    region_mapping_fail("ELF and guard boundary");
  if(region_before.stack_guard_start + PGSIZE != region_before.stack_bottom)
    region_mapping_fail("guard and stack boundary");
  if(region_before.stack_bottom + PGSIZE != region_before.stack_top)
    region_mapping_fail("fixed one-page stack");
  if(region_before.stack_top != region_before.dynamic_start)
    region_mapping_fail("stack and dynamic boundary");
  if(region_before.process_size < region_before.dynamic_start)
    region_mapping_fail("dynamic extent below start");

  region_mapping_query(region_before.image_start, &image);
  region_mapping_query(region_before.stack_guard_start, &guard);
  region_mapping_query(region_before.stack_bottom, &stack);
  if(!image.present || (image.flags & (PTE_U | PTE_X)) != (PTE_U | PTE_X))
    region_mapping_fail("ELF representative leaf permissions");
  if(!guard.present || (guard.flags & PTE_U) != 0)
    region_mapping_fail("guard remains user accessible");
  if(!stack.present || (stack.flags & (PTE_U | PTE_W)) != (PTE_U | PTE_W))
    region_mapping_fail("stack representative leaf permissions");

  region_mapping_capture_deep_stack(4);
  if(!region_deep_stack.user_stack_valid)
    region_mapping_fail("deep stack invalid");
  if(region_deep_stack.stack_bottom != region_before.stack_bottom ||
     region_deep_stack.stack_top != region_before.stack_top)
    region_mapping_fail("stack bounds changed during recursion");
  if(region_deep_stack.user_sp >= region_before.user_sp)
    region_mapping_fail("deeper stack did not move toward lower VA");
  if(region_deep_stack.stack_used <= region_before.stack_used)
    region_mapping_fail("deeper stack did not consume more bytes");

  char *dynamic_base = sbrk(PGSIZE);
  if(dynamic_base == (char *)-1)
    region_mapping_fail("sbrk grow");
  if((uint64)dynamic_base != region_before.process_size)
    region_mapping_fail("sbrk old break");
  if(memsnapshot(MEMVIZ_VIEW_USER, &region_lazy) < 0)
    region_mapping_fail("lazy growth snapshot");
  if(region_lazy.process_size != region_before.process_size + PGSIZE)
    region_mapping_fail("p->sz did not grow upward by one page");
  if(region_lazy.dynamic_page_count != region_before.dynamic_page_count + 1)
    region_mapping_fail("dynamic page count after sbrk");
  if(region_lazy.dynamic_lazy_pages != region_before.dynamic_lazy_pages + 1)
    region_mapping_fail("untouched growth is not lazy");

  region_mapping_query((uint64)dynamic_base, &dynamic);
  if(dynamic.present)
    region_mapping_fail("untouched lazy page already has a leaf");

  dynamic_base[0] = 0x5a;
  region_mapping_query((uint64)dynamic_base, &dynamic);
  if(!dynamic.present ||
     (dynamic.flags & (PTE_U | PTE_W)) != (PTE_U | PTE_W))
    region_mapping_fail("touched dynamic page permissions");
  if(memsnapshot(MEMVIZ_VIEW_USER, &region_resident) < 0)
    region_mapping_fail("resident growth snapshot");
  if(region_resident.dynamic_resident_pages !=
     region_lazy.dynamic_resident_pages + 1)
    region_mapping_fail("touched page did not become resident");
  if(region_resident.dynamic_lazy_pages + 1 != region_lazy.dynamic_lazy_pages)
    region_mapping_fail("lazy count did not decrease after touch");

  if(sbrk(-PGSIZE) == (char *)-1)
    region_mapping_fail("sbrk shrink");
  if(memsnapshot(MEMVIZ_VIEW_USER, &region_restored) < 0)
    region_mapping_fail("restored snapshot");
  if(region_restored.process_size != region_before.process_size ||
     region_restored.dynamic_page_count != region_before.dynamic_page_count)
    region_mapping_fail("dynamic range not restored");
  region_mapping_query((uint64)dynamic_base, &dynamic);
  if(dynamic.present)
    region_mapping_fail("shrunk dynamic leaf remains mapped");

  printf("regionmapping: translation=Sv39 paging; textbook base/bounds segments=absent\n");
  printf("regionmapping: ELF [%p, %p) flags=", region_before.image_start,
         region_before.image_end);
  region_mapping_print_flags(image.flags);
  printf("\n");
  printf("regionmapping: guard [%p, %p) flags=", region_before.stack_guard_start,
         region_before.stack_bottom);
  region_mapping_print_flags(guard.flags);
  printf("\n");
  printf("regionmapping: stack [%p, %p) shallow-sp=%p deep-sp=%p direction=down\n",
         region_before.stack_bottom, region_before.stack_top,
         region_before.user_sp, region_deep_stack.user_sp);
  printf("regionmapping: dynamic old-end=%p lazy-end=%p touched-pa=%p direction=up\n",
         region_before.process_size, region_lazy.process_size, dynamic.pa);
  printf("regionmapping: external fragmentation is not reproduced by page-granular allocation\n");
  printf("regionmapping: OK\n");
}

#endif
