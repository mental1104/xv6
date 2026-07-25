#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"
#include "kernel/memviz.h"
#include "user/user.h"

#define PGTBL_L1_SPAN (512ULL * PGSIZE)
#define PGTBL_SEARCH_REGIONS 64

// 大型快照和查询结果放在 BSS，避免占用 xv6 单页用户栈。
static struct memviz_snapshot snapshot;
static struct memviz_va_query query;

/** 输出稳定失败原因并终止当前测试进程。 */
static void
fail(char *message)
{
  printf("pgtbltest: FAIL: %s\n", message);
  exit(1);
}

/**
 * 读取并输出一个用户 VA 的真实 Sv39 三级链路。
 *
 * @param label 本轮实验中的路径标签。
 * @param va 待查询的用户虚拟地址。
 */
static void
print_path(char *label, uint64 va)
{
  if(vaquery(va, &query) < 0)
    fail("vaquery");

  printf("PGTBL path label=%s va=%p present=%d\n",
         label, query.va, query.present);
  for(int i = 0; i < 3; i++){
    struct memviz_pte_level *level = &query.levels[i];
    char *kind = !level->present ? "missing" :
                 (level->level == 0 ? "leaf" : "branch");

    printf("PGTBL level label=%s level=L%d index=%d present=%d kind=%s pte=%p pa=%p\n",
           label, level->level, level->index, level->present, kind,
           level->pte, level->pa);
    if(!level->present)
      break;
  }
}

/**
 * 在同一个 L2 分支中寻找两个尚未分配 L0 页表的相邻 2 MiB 区域。
 *
 * @param old_break 当前进程逻辑大小的一过地址。
 * @return 第一段区域的起始 VA；无合适区域时终止测试。
 */
static uint64
find_empty_l1_region(uint64 old_break)
{
  uint64 candidate = (old_break + PGTBL_L1_SPAN - 1) &
                     ~(PGTBL_L1_SPAN - 1);

  for(int attempt = 0; attempt < PGTBL_SEARCH_REGIONS; attempt++){
    uint64 sparse = candidate + PGTBL_L1_SPAN;
    if(sparse < candidate || sparse + PGSIZE > USERMAX)
      break;
    if(PX(2, candidate) != PX(2, sparse)){
      candidate += PGTBL_L1_SPAN;
      continue;
    }

    if(vaquery(candidate, &query) < 0)
      fail("dense candidate query");
    int dense_missing_l1 = query.levels[0].present &&
                           !query.levels[1].present;

    if(vaquery(sparse, &query) < 0)
      fail("sparse candidate query");
    int sparse_missing_l1 = query.levels[0].present &&
                            !query.levels[1].present;

    if(dense_missing_l1 && sparse_missing_l1)
      return candidate;
    candidate += PGTBL_L1_SPAN;
  }

  fail("no empty L1 region");
  return 0;
}

/**
 * 在子进程中构造密集与稀疏映射，并验证创建、缩容和失败回滚。
 *
 * sbrk() 正向扩容只修改逻辑范围。首次写入经 lazy fault 创建数据页和缺失的
 * 中间页表页；缩容释放 leaf 数据页，但当前 xv6 不立即压缩空中间页表子树。
 */
static void
run_mapping_experiment(void)
{
  struct path_identity {
    uint64 l1_table;
    uint64 l0_table;
    uint64 leaf_pa;
  } dense0, dense1, sparse0;

  // 先写热 COW 缓冲与栈，再记录基线，避免 fork 自身污染物理页差值。
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &snapshot) < 0 ||
     vaquery(0, &query) < 0)
    fail("warmup");

  uint64 old_break = (uint64)sbrk(0);
  uint64 dense_va = find_empty_l1_region(old_break);
  uint64 sparse_va = dense_va + PGTBL_L1_SPAN;
  uint64 reserve_end = sparse_va + PGSIZE;
  uint64 reserve_size = reserve_end - old_break;
  if(reserve_size > 0x7fffffffULL)
    fail("reservation exceeds sbrk argument range");

  if(memsnapshot(MEMVIZ_VIEW_PHYS, &snapshot) < 0)
    fail("baseline snapshot");
  uint64 baseline_free = snapshot.free_pages;

  if(sbrk((int)reserve_size) == (char *)-1)
    fail("lazy reserve");
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &snapshot) < 0)
    fail("reserve snapshot");
  if(snapshot.free_pages != baseline_free)
    fail("untouched reservation allocated pages");
  printf("PGTBL reserve lazy=1 free_unchanged=1 bytes=%d\n",
         (int)reserve_size);

  print_path("missing", dense_va);
  if(!query.levels[0].present || query.levels[1].present)
    fail("missing intermediate path mismatch");

  *(volatile char *)dense_va = 0x11;
  *(volatile char *)(dense_va + PGSIZE) = 0x22;
  *(volatile char *)sparse_va = 0x33;

  print_path("dense0", dense_va);
  if(!query.present)
    fail("dense0 leaf missing");
  dense0.l1_table = query.levels[0].pa;
  dense0.l0_table = query.levels[1].pa;
  dense0.leaf_pa = query.levels[2].pa;

  print_path("dense1", dense_va + PGSIZE);
  if(!query.present)
    fail("dense1 leaf missing");
  dense1.l1_table = query.levels[0].pa;
  dense1.l0_table = query.levels[1].pa;
  dense1.leaf_pa = query.levels[2].pa;

  print_path("sparse0", sparse_va);
  if(!query.present)
    fail("sparse leaf missing");
  sparse0.l1_table = query.levels[0].pa;
  sparse0.l0_table = query.levels[1].pa;
  sparse0.leaf_pa = query.levels[2].pa;

  int dense_shared_l1 = dense0.l1_table == dense1.l1_table;
  int dense_shared_l0 = dense0.l0_table == dense1.l0_table;
  int dense_distinct_leaf = dense0.leaf_pa != dense1.leaf_pa;
  if(!dense_shared_l1 || !dense_shared_l0 || !dense_distinct_leaf)
    fail("dense sharing mismatch");
  printf("PGTBL relation=dense shared_l1_table=%d shared_l0_table=%d distinct_leaf=%d\n",
         dense_shared_l1, dense_shared_l0, dense_distinct_leaf);

  int sparse_shared_l1 = dense0.l1_table == sparse0.l1_table;
  int sparse_shared_l0 = dense0.l0_table == sparse0.l0_table;
  if(!sparse_shared_l1 || sparse_shared_l0)
    fail("sparse sharing mismatch");
  printf("PGTBL relation=sparse shared_l1_table=%d shared_l0_table=%d\n",
         sparse_shared_l1, sparse_shared_l0);

  if(memsnapshot(MEMVIZ_VIEW_PHYS, &snapshot) < 0)
    fail("touched snapshot");
  uint64 touched_free = snapshot.free_pages;
  if(touched_free >= baseline_free)
    fail("touch consumed no physical pages");

  if(sbrk(-(int)reserve_size) == (char *)-1)
    fail("shrink");
  print_path("after-shrink", dense_va);
  int leaf_removed = !query.present && !query.levels[2].present;
  int intermediate_retained = query.levels[0].present &&
                              query.levels[1].present;

  if(memsnapshot(MEMVIZ_VIEW_PHYS, &snapshot) < 0)
    fail("shrink snapshot");
  uint64 shrink_free = snapshot.free_pages;
  int data_reclaimed = shrink_free >= touched_free + 3;
  if(!leaf_removed || !intermediate_retained || !data_reclaimed ||
     shrink_free >= baseline_free)
    fail("shrink lifecycle mismatch");
  printf("PGTBL release leaf_removed=%d intermediate_retained=%d data_reclaimed=%d\n",
         leaf_removed, intermediate_retained, data_reclaimed);

  uint64 rollback_break = (uint64)sbrk(0);
  uint64 rollback_free = shrink_free;
  if(sbrk(-0x7fffffff) != (char *)-1)
    fail("overshrink unexpectedly succeeded");
  if((uint64)sbrk(0) != rollback_break)
    fail("overshrink changed break");
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &snapshot) < 0)
    fail("rollback snapshot");
  if(snapshot.free_pages != rollback_free)
    fail("overshrink changed physical pages");
  printf("PGTBL rollback operation=overshrink break_unchanged=1 free_unchanged=1\n");
}

/**
 * 父进程验证子进程退出后，freewalk() 会递归回收残留中间页表页。
 */
static void
test_process_exit_reclaim(void)
{
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &snapshot) < 0)
    fail("parent baseline snapshot");
  uint64 baseline_free = snapshot.free_pages;

  int pid = fork();
  if(pid < 0)
    fail("fork");
  if(pid == 0){
    run_mapping_experiment();
    exit(0);
  }

  int status = -1;
  if(wait(&status) != pid || status != 0)
    fail("mapping experiment child");
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &snapshot) < 0)
    fail("parent final snapshot");
  if(snapshot.free_pages != baseline_free)
    fail("process exit did not restore physical pages");

  printf("PGTBL reclaim process_exit=1 free_restored=1\n");
  printf("PGTBL result=ok\n");
}

int
main(void)
{
  test_process_exit_reclaim();
  exit(0);
}
