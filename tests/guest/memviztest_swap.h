#ifndef XV6_MEMVIZTEST_SWAP_H
#define XV6_MEMVIZTEST_SWAP_H

#include "kernel/types.h"
#include "kernel/memviz.h"
#include "kernel/swap.h"
#include "user/user.h"

#define SWAP_TEST_PGSIZE 4096L
#define SWAP_TEST_PGROUNDUP(value) \
  (((value) + SWAP_TEST_PGSIZE - 1) & ~(SWAP_TEST_PGSIZE - 1))

static struct swap_info swap_before;
static struct swap_info swap_swapped;
static struct swap_info swap_resident;
static struct swap_info swap_released;
static struct memviz_va_query swap_query;

/** Print a stable failure oracle and terminate the guest test. */
static void
swap_test_fail(char *message)
{
  printf("swaptest: FAIL: %s\n", message);
  exit(1);
}

/** Return the deterministic byte stored at one offset of the test page. */
static char
swap_test_byte(int offset)
{
  return (char)((offset * 37 + 11) & 0xff);
}

/** Reserve one page-aligned anonymous dynamic page and return its virtual address. */
static char *
swap_test_allocate(uint64 *old_break, int *growth)
{
  *old_break = (uint64)sbrk(0);
  uint64 page_va = SWAP_TEST_PGROUNDUP(*old_break);
  uint64 growth64 = page_va + SWAP_TEST_PGSIZE - *old_break;
  if(growth64 > 0x7fffffffULL)
    swap_test_fail("growth exceeds sbrk ABI");

  *growth = (int)growth64;
  if((uint64)sbrk(*growth) != *old_break)
    swap_test_fail("sbrk reserve");
  return (char *)page_va;
}

/** Fill and later verify every byte so page-in cannot pass by restoring zeros. */
static void
swap_test_fill(char *page)
{
  for(int offset = 0; offset < SWAP_TEST_PGSIZE; offset++)
    page[offset] = swap_test_byte(offset);
}

static void
swap_test_verify(char *page)
{
  for(int offset = 0; offset < SWAP_TEST_PGSIZE; offset++){
    if(page[offset] != swap_test_byte(offset))
      swap_test_fail("data preservation");
  }
}

/** Verify one resident -> swapped -> faulted-in -> released lifecycle. */
static void
swap_test_cycle(void)
{
  uint64 old_break;
  int growth;
  char *page = swap_test_allocate(&old_break, &growth);
  swap_test_fill(page);

  if(swapinfo(page, &swap_before) < 0 ||
     swap_before.page_state != SWAP_PAGE_RESIDENT)
    swap_test_fail("resident oracle");
  if(swapout((void *)0) != -1)
    swap_test_fail("ELF page accepted as anonymous dynamic page");

  if(swapout(page) < 0)
    swap_test_fail("swapout");
  if(swapinfo(page, &swap_swapped) < 0 ||
     swap_swapped.page_state != SWAP_PAGE_SWAPPED ||
     swap_swapped.slot < 0 || swap_swapped.slot >= NSWAP ||
     swap_swapped.used_slots != swap_before.used_slots + 1 ||
     swap_swapped.page_outs != swap_before.page_outs + 1)
    swap_test_fail("non-resident oracle");
  if(vaquery((uint64)page, &swap_query) < 0 || swap_query.present)
    swap_test_fail("swapped PTE still present");

  // The first load is the actual user page fault that restores the page.
  swap_test_verify(page);
  if(swapinfo(page, &swap_resident) < 0 ||
     swap_resident.page_state != SWAP_PAGE_RESIDENT ||
     swap_resident.used_slots != swap_before.used_slots ||
     swap_resident.page_ins != swap_before.page_ins + 1)
    swap_test_fail("page-in oracle");
  if(vaquery((uint64)page, &swap_query) < 0 || !swap_query.present)
    swap_test_fail("page-in mapping missing");

  if(swapout(page) < 0)
    swap_test_fail("second swapout");
  if((uint64)sbrk(-growth) != (uint64)page + SWAP_TEST_PGSIZE)
    swap_test_fail("sbrk release");
  if((uint64)sbrk(0) != old_break)
    swap_test_fail("break not restored");
  if(swapinfo(page, &swap_released) < 0 ||
     swap_released.page_state != SWAP_PAGE_UNMAPPED ||
     swap_released.used_slots != swap_before.used_slots)
    swap_test_fail("slot cleanup oracle");

  printf("SWAP cycle state=resident->swapped(slot=%d)->resident data=OK cleanup=OK\n",
         swap_swapped.slot);
}

/** Verify fork refcounts one immutable slot while materializing private pages. */
static void
swap_test_fork_ownership(void)
{
  uint64 old_break;
  int growth;
  char *page = swap_test_allocate(&old_break, &growth);
  swap_test_fill(page);

  struct swap_info baseline;
  struct swap_info parent_snapshot;
  if(swapinfo(page, &baseline) < 0)
    swap_test_fail("fork baseline");
  if(swapout(page) < 0)
    swap_test_fail("fork parent swapout");
  if(swapinfo(page, &parent_snapshot) < 0 ||
     parent_snapshot.page_state != SWAP_PAGE_SWAPPED ||
     parent_snapshot.used_slots != baseline.used_slots + 1)
    swap_test_fail("fork parent swapped oracle");

  int pid = fork();
  if(pid < 0)
    swap_test_fail("fork");
  if(pid == 0){
    struct swap_info child_swapped;
    struct swap_info child_resident;
    if(swapinfo(page, &child_swapped) < 0 ||
       child_swapped.page_state != SWAP_PAGE_SWAPPED ||
       child_swapped.slot != parent_snapshot.slot ||
       child_swapped.used_slots != baseline.used_slots + 1)
      exit(2);

    swap_test_verify(page);
    if(swapinfo(page, &child_resident) < 0 ||
       child_resident.page_state != SWAP_PAGE_RESIDENT ||
       child_resident.used_slots != baseline.used_slots + 1)
      exit(3);
    page[0] = 'C';
    exit(0);
  }

  int status = -1;
  if(wait(&status) != pid || status != 0)
    swap_test_fail("fork child");

  struct swap_info parent_swapped;
  if(swapinfo(page, &parent_swapped) < 0 ||
     parent_swapped.page_state != SWAP_PAGE_SWAPPED ||
     parent_swapped.slot != parent_snapshot.slot ||
     parent_swapped.used_slots != baseline.used_slots + 1)
    swap_test_fail("parent slot after child exit");
  swap_test_verify(page);
  if(page[0] == 'C')
    swap_test_fail("fork isolation");

  struct swap_info parent_resident;
  if(swapinfo(page, &parent_resident) < 0 ||
     parent_resident.page_state != SWAP_PAGE_RESIDENT ||
     parent_resident.used_slots != baseline.used_slots)
    swap_test_fail("parent page-in cleanup");
  if((uint64)sbrk(-growth) != (uint64)page + SWAP_TEST_PGSIZE ||
     (uint64)sbrk(0) != old_break)
    swap_test_fail("fork test break cleanup");

  printf("SWAP fork backing_slot=shared-immutable materialized_pages=private isolation=OK\n");
}

/** Verify that exit releases a non-resident page without first faulting it in. */
static void
swap_test_exit_cleanup(void)
{
  struct swap_info baseline;
  if(swapinfo((void *)0, &baseline) < 0)
    swap_test_fail("exit baseline");

  int pid = fork();
  if(pid < 0)
    swap_test_fail("cleanup fork");
  if(pid == 0){
    uint64 old_break;
    int growth;
    char *page = swap_test_allocate(&old_break, &growth);
    (void)old_break;
    (void)growth;
    swap_test_fill(page);
    if(swapout(page) < 0)
      exit(4);
    exit(0);
  }

  int status = -1;
  if(wait(&status) != pid || status != 0)
    swap_test_fail("cleanup child");

  struct swap_info after;
  if(swapinfo((void *)0, &after) < 0 ||
     after.used_slots != baseline.used_slots)
    swap_test_fail("exit slot leak");
  printf("SWAP exit nonresident_slot_released=OK\n");
}

/** Run the complete deterministic swap mechanism experiment. */
static void
memviztest_swap(void)
{
  swap_test_cycle();
  swap_test_fork_ownership();
  swap_test_exit_cleanup();
  printf("SWAP policy=explicit teaching trigger automatic_replacement=absent\n");
  printf("swaptest: OK\n");
}

#undef SWAP_TEST_PGROUNDUP
#undef SWAP_TEST_PGSIZE

#endif
