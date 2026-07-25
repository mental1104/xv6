#include "kernel/types.h"
#include "user/user.h"
#include "user/vmpolicy.h"

static struct vm_policy_result compare_fifo;
static struct vm_policy_result compare_clock;
static struct vm_policy_result locality_fit;
static struct vm_policy_result locality_pressure;

/** 构造能让 FIFO 与 CLOCK 在第二次置换时选择不同 victim 的访问序列。 */
static void
build_compare_workload(struct vm_policy_workload *workload)
{
  static int pages[] = {0, 1, 2, 3, 1, 4};
  static int writes[] = {1, 0, 0, 0, 0, 1};

  memset(workload, 0, sizeof(*workload));
  workload->label = "policy-contrast";
  workload->page_count = 5;
  workload->frame_count = 3;
  workload->access_count = sizeof(pages) / sizeof(pages[0]);
  for(int i = 0; i < workload->access_count; i++){
    workload->accesses[i].page = pages[i];
    workload->accesses[i].write = writes[i];
  }
}

/** 构造工作集恰好能驻留的高局部性访问序列。 */
static void
build_fit_workload(struct vm_policy_workload *workload)
{
  static int pages[] = {0, 1, 2, 0, 1, 2, 0, 1, 2};

  memset(workload, 0, sizeof(*workload));
  workload->label = "working-set-fits";
  workload->page_count = 3;
  workload->frame_count = 3;
  workload->access_count = sizeof(pages) / sizeof(pages[0]);
  for(int i = 0; i < workload->access_count; i++){
    workload->accesses[i].page = pages[i];
    workload->accesses[i].write = i == 0;
  }
}

/** 使用同一循环工作集但只提供两个页框，形成确定性的持续换页轨迹。 */
static void
build_pressure_workload(struct vm_policy_workload *workload)
{
  build_fit_workload(workload);
  workload->label = "working-set-exceeds";
  workload->frame_count = 2;
}

/** 执行并输出一轮实验；任何机制、计数或清理 oracle 失败都立即终止。 */
static void
run_or_die(enum vm_policy_kind kind,
           struct vm_policy_workload *workload,
           struct vm_policy_result *result)
{
  if(vm_policy_run(kind, workload, result) < 0){
    printf("vmpolicy: FAIL label=%s policy=%s error=%s step=%d\n",
           workload->label, vm_policy_name(kind),
           vm_policy_error_name(result->error), result->error_step);
    exit(1);
  }
  vm_policy_print(workload, kind, result);
}

/** 输出 FIFO 与 CLOCK 在同一输入下的真实换出与不同 victim 决策。 */
static void
run_compare(void)
{
  struct vm_policy_workload workload;
  build_compare_workload(&workload);

  run_or_die(VM_POLICY_FIFO, &workload, &compare_fifo);
  run_or_die(VM_POLICY_CLOCK, &workload, &compare_clock);
  printf("VMPOLICY contrast first_victim=%d fifo_second=%d clock_second=%d\n",
         compare_fifo.trace[3].victim, compare_fifo.trace[5].victim,
         compare_clock.trace[5].victim);
}

/** 用计数而非单次耗时比较工作集可驻留与超额两种状态。 */
static void
run_locality(void)
{
  struct vm_policy_workload workload;
  build_fit_workload(&workload);
  run_or_die(VM_POLICY_FIFO, &workload, &locality_fit);

  build_pressure_workload(&workload);
  run_or_die(VM_POLICY_FIFO, &workload, &locality_pressure);
  printf("VMPOLICY locality fit_misses=%d pressure_misses=%d fit_page_ins=%d pressure_page_ins=%d\n",
         locality_fit.misses, locality_pressure.misses,
         locality_fit.page_ins, locality_pressure.page_ins);
}

/** 打印稳定命令行合同。 */
static void
usage(void)
{
  printf("Usage: vmpolicy [all|compare|locality]\n");
}

int
main(int argc, char **argv)
{
  if(argc == 1 || (argc == 2 && strcmp(argv[1], "all") == 0)){
    run_compare();
    run_locality();
  } else if(argc == 2 && strcmp(argv[1], "compare") == 0){
    run_compare();
  } else if(argc == 2 && strcmp(argv[1], "locality") == 0){
    run_locality();
  } else {
    usage();
    exit(2);
  }

  printf("vmpolicy: OK\n");
  exit(0);
}
