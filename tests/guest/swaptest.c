#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/memviz.h"
#include "kernel/swap.h"
#include "user/user.h"

static struct memviz_snapshot snapshot;
static struct swap_page_info info;

/** 输出稳定失败原因并以非零状态终止测试。 */
static void
fail(char *message)
{
  printf("swaptest: FAIL: %s\n", message);
  exit(1);
}

/**
 * 读取当前 kalloc 空闲页数。
 *
 * @return 所有 CPU freelist 中可立即分配的物理页总数。
 */
static uint64
free_pages(void)
{
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &snapshot) < 0)
    fail("physical snapshot");
  return snapshot.free_pages;
}

/**
 * 验证一个匿名页完成 resident -> swapped -> resident 的最小闭环。
 *
 * 当前文件先固定 guest 侧 oracle 与稳定输出；内核系统调用接入后该入口才会
 * 注册到镜像和 xv6test，避免在功能未闭环前产生假阳性测试。
 */
static void
check_basic_cycle(void)
{
  uint64 before = free_pages();
  char *page = sbrk(PGSIZE);
  if(page == (char *)-1)
    fail("sbrk");

  page[0] = 0x5a;
  page[PGSIZE - 1] = 0x6b;
  if(swapquery((uint64)page, &info) < 0 ||
     info.state != SWAP_PAGE_RESIDENT)
    fail("resident query");

  if(swapout((uint64)page) < 0)
    fail("swapout");
  if(swapquery((uint64)page, &info) < 0 ||
     info.state != SWAP_PAGE_SWAPPED || info.slot < 0)
    fail("swapped query");
  if(free_pages() < before)
    fail("pageout did not release memory");

  if(page[0] != 0x5a || page[PGSIZE - 1] != 0x6b)
    fail("pagein data");
  if(swapquery((uint64)page, &info) < 0 ||
     info.state != SWAP_PAGE_RESIDENT || info.slot != -1)
    fail("pagein query");

  if(sbrk(-PGSIZE) == (char *)-1)
    fail("shrink");
  printf("SWAP basic data_preserved=1 pageout=1 pagein=1\n");
}

int
main(void)
{
  check_basic_cycle();
  printf("SWAP result=ok\n");
  exit(0);
}
