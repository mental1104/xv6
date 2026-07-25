#include "kernel/types.h"
#include "user/user.h"
#include "user/vmpolicy.h"

static struct vm_policy_result fifo_result;
static struct vm_policy_result clock_result;
static struct vm_policy_result fit_result;
static struct vm_policy_result pressure_result;
static struct vm_policy_result repeat_result;

/** 输出稳定失败原因并以非零状态终止 guest 测试。 */
static void
fail(char *message)
{
  printf("vmpolicytest: FAIL: %s\n", message);
  exit(1);
}

/** 构造 FIFO 与 CLOCK 的最小分歧序列。 */
static void
build_compare_workload(struct vm_policy_workload *workload)
{
  static int pages[] = {0, 1, 2, 3, 1, 4};

  memset(workload, 0, sizeof(*workload));
  workload->label = "policy-contrast";
  workload->page_count = 5;
  workload->frame_count = 3;
  workload->access_count = sizeof(pages) / sizeof(pages[0]);
  for(int i = 0; i < workload->access_count; i++){
    workload->accesses[i].page = pages[i];
    workload->accesses[i].write = i == 0 || i == 5;
  }
}

/** 构造三个页面循环访问的固定工作集。 */
static void
build_locality_workload(struct vm_policy_workload *workload, int frames)
{
  static int pages[] = {0, 1, 2, 0, 1, 2, 0, 1, 2};

  memset(workload, 0, sizeof(*workload));
  workload->label = frames == 3 ? "working-set-fits" : "working-set-exceeds";
  workload->page_count = 3;
  workload->frame_count = frames;
  workload->access_count = sizeof(pages) / sizeof(pages[0]);
  for(int i = 0; i < workload->access_count; i++){
    workload->accesses[i].page = pages[i];
    workload->accesses[i].write = i == 0;
  }
}

/** 执行一轮实验并把内部错误阶段转成 guest 失败。 */
static void
run_checked(enum vm_policy_kind kind,
            struct vm_policy_workload *workload,
            struct vm_policy_result *result)
{
  if(vm_policy_run(kind, workload, result) < 0){
    printf("vmpolicytest: diagnostic label=%s policy=%s error=%s step=%d\n",
           workload->label, vm_policy_name(kind),
           vm_policy_error_name(result->error), result->error_step);
    fail("experiment execution");
  }
}

/**
 * 验证相同输入下 FIFO 与 CLOCK 的第二次 victim 不同。
 *
 * 正向 oracle 是 step 5 的 victim 分别为 1 和 2；负向 oracle 是 step 4
 * 命中页面 1 时不得产生 victim 或额外 page-out。
 */
static void
test_policy_divergence(void)
{
  struct vm_policy_workload workload;
  build_compare_workload(&workload);
  run_checked(VM_POLICY_FIFO, &workload, &fifo_result);
  run_checked(VM_POLICY_CLOCK, &workload, &clock_result);

  if(fifo_result.evictions != 2 || clock_result.evictions != 2 ||
     fifo_result.page_outs != 2 || clock_result.page_outs != 2)
    fail("replacement count");
  if(fifo_result.trace[3].victim != 0 ||
     clock_result.trace[3].victim != 0)
    fail("first victim");
  if(!fifo_result.trace[4].hit || fifo_result.trace[4].victim != -1 ||
     !clock_result.trace[4].hit || clock_result.trace[4].victim != -1)
    fail("hit triggered eviction");
  if(fifo_result.trace[5].victim != 1 ||
     clock_result.trace[5].victim != 2)
    fail("policy distinction");

  printf("VMPOLICYTEST contrast fifo_second=1 clock_second=2 hit_no_evict=OK\n");
}

/**
 * 验证工作集能驻留时热循环不再缺页，容量不足时每次访问都发生 miss。
 *
 * 该用例使用 miss、page-out 和 page-in 计数，不把一次运行耗时当作抖动证据。
 */
static void
test_working_set_pressure(void)
{
  struct vm_policy_workload workload;
  build_locality_workload(&workload, 3);
  run_checked(VM_POLICY_FIFO, &workload, &fit_result);
  build_locality_workload(&workload, 2);
  run_checked(VM_POLICY_FIFO, &workload, &pressure_result);

  if(fit_result.misses != 3 || fit_result.hits != 6 ||
     fit_result.evictions != 0 || fit_result.page_outs != 0 ||
     fit_result.page_ins != 0)
    fail("fitting working set");
  if(pressure_result.misses != 9 || pressure_result.hits != 0 ||
     pressure_result.evictions != 7 || pressure_result.page_outs != 7 ||
     pressure_result.page_ins != 6)
    fail("over-capacity working set");
  if(pressure_result.misses <= fit_result.misses ||
     pressure_result.page_ins <= fit_result.page_ins)
    fail("pressure did not increase paging");

  printf("VMPOLICYTEST locality fit_misses=3 pressure_misses=9 pressure_page_ins=6\n");
}

/** 重复运行同一 CLOCK 序列，验证决策确定且上一轮未泄漏 swap slot。 */
static void
test_repeatability(void)
{
  struct vm_policy_workload workload;
  build_compare_workload(&workload);
  run_checked(VM_POLICY_CLOCK, &workload, &repeat_result);

  if(repeat_result.trace[3].victim != clock_result.trace[3].victim ||
     repeat_result.trace[5].victim != clock_result.trace[5].victim ||
     repeat_result.evictions != clock_result.evictions ||
     repeat_result.page_outs != clock_result.page_outs)
    fail("repeatability");
  printf("VMPOLICYTEST repeat clock_victims=0,2 cleanup=OK\n");
}

/** 拒绝越界页面下标，避免错误输入被解释成策略结果。 */
static void
test_invalid_input(void)
{
  struct vm_policy_workload workload;
  memset(&workload, 0, sizeof(workload));
  workload.label = "invalid";
  workload.page_count = 2;
  workload.frame_count = 1;
  workload.access_count = 1;
  workload.accesses[0].page = 2;

  if(vm_policy_run(VM_POLICY_FIFO, &workload, &repeat_result) != -1 ||
     repeat_result.error != VM_POLICY_ERROR_ARGUMENT ||
     repeat_result.accesses != 0)
    fail("invalid input accepted");
  printf("VMPOLICYTEST invalid rejected=OK\n");
}

int
main(void)
{
  test_policy_divergence();
  test_working_set_pressure();
  test_repeatability();
  test_invalid_input();
  printf("vmpolicytest: OK\n");
  exit(0);
}
