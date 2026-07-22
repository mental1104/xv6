#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/**
 * 描述一个由 xv6 用户态执行的回归测试。
 *
 * group 用于按 Lab 或能力筛选；name 是稳定且全局唯一的测试名称；argv
 * 是传给 exec() 的空指针结尾参数数组。测试语义仍由目标程序拥有，本入口
 * 只负责注册、进程隔离、退出状态传播和统一结果协议。
 */
struct xv6_test_case {
  char *group;
  char *name;
  char **argv;
};

static char *lab3_copyin_argv[] = {"usertests", "copyin", 0};
static char *lab3_copyout_argv[] = {"usertests", "copyout", 0};
static char *lab3_copyinstr_argv[] = {"usertests", "copyinstr1", 0};
static char *lab3_sbrkmuch_argv[] = {"usertests", "sbrkmuch", 0};
static char *lab5_lazytests_argv[] = {"lazytests", 0};
static char *lab6_cowtest_argv[] = {"cowtest", 0};

// 首批只迁移退出状态可作为可靠判据的 Lab3、Lab5 和 Lab6 测试。
static struct xv6_test_case tests[] = {
  {"lab3", "lab3-copyin", lab3_copyin_argv},
  {"lab3", "lab3-copyout", lab3_copyout_argv},
  {"lab3", "lab3-copyinstr1", lab3_copyinstr_argv},
  {"lab3", "lab3-sbrkmuch", lab3_sbrkmuch_argv},
  {"lab5", "lab5-lazytests", lab5_lazytests_argv},
  {"lab6", "lab6-cowtest", lab6_cowtest_argv},
  {0, 0, 0},
};

/**
 * 输出 xv6test 支持的命令行形式。
 *
 * @param program argv[0] 中的程序名称，仅用于错误提示。
 * @return 无；本函数只向标准错误写入用法说明。
 */
static void
usage(char *program)
{
  fprintf(2, "Usage: %s [--all | --list | --group name | --run test]\n", program);
}

/**
 * 判断测试是否满足当前 group 或 name 过滤条件。
 *
 * @param test 待检查的静态测试描述，不会被修改。
 * @param group_filter group 精确匹配条件；为空时不限制 group。
 * @param name_filter 测试名称精确匹配条件；为空时不限制名称。
 * @return 同时满足全部非空过滤条件时返回 1，否则返回 0。
 */
static int
matches_filter(struct xv6_test_case *test, char *group_filter, char *name_filter)
{
  if(group_filter != 0 && strcmp(test->group, group_filter) != 0)
    return 0;
  if(name_filter != 0 && strcmp(test->name, name_filter) != 0)
    return 0;
  return 1;
}

/**
 * 输出当前镜像内注册的全部 guest 测试。
 *
 * @return 已列出的测试数量；调用者可据此检查注册表是否为空。
 */
static int
list_tests(void)
{
  int count = 0;

  for(struct xv6_test_case *test = tests; test->name != 0; test++){
    printf("XV6TEST case name=%s group=%s command=%s\n",
           test->name, test->group, test->argv[0]);
    count++;
  }
  printf("XV6TEST listed total=%d\n", count);
  return count;
}

/**
 * 在独立子进程中执行一个已注册测试，并把退出状态转换为统一协议。
 *
 * @param test 待执行的测试描述；argv 必须以空指针结尾。
 * @param ordinal 本轮筛选结果中的一基序号，用于稳定标识输出行。
 * @return 子进程正常退出且状态为 0 时返回 1；fork、exec、wait 或测试失败
 *         时返回 0。函数不会保留子进程资源。
 */
static int
run_test(struct xv6_test_case *test, int ordinal)
{
  int pid;
  int waited_pid;
  int status = 0;

  printf("XV6TEST run %d - %s group=%s\n", ordinal, test->name, test->group);
  pid = fork();
  if(pid < 0){
    printf("XV6TEST not ok %d - %s reason=fork\n", ordinal, test->name);
    return 0;
  }

  if(pid == 0){
    // exec() 成功后测试程序接管当前子进程，其 exit status 直接成为测试结果。
    exec(test->argv[0], test->argv);
    printf("XV6TEST diagnostic name=%s exec=%s failed\n",
           test->name, test->argv[0]);
    exit(1);
  }

  // 每轮只创建一个直接子进程，因此 wait() 必须回收刚启动的测试进程。
  waited_pid = wait(&status);
  if(waited_pid != pid){
    printf("XV6TEST not ok %d - %s reason=wait expected=%d actual=%d\n",
           ordinal, test->name, pid, waited_pid);
    return 0;
  }
  if(status != 0){
    printf("XV6TEST not ok %d - %s status=%d\n",
           ordinal, test->name, status);
    return 0;
  }

  printf("XV6TEST ok %d - %s\n", ordinal, test->name);
  return 1;
}

/**
 * 执行满足过滤条件的测试集合，并输出稳定的开始、汇总和结束协议。
 *
 * @param group_filter group 精确匹配条件；为空时选择全部 group。
 * @param name_filter 测试名称精确匹配条件；为空时选择全部名称。
 * @return 至少选中一个测试且全部通过时返回 0；空选择或任一失败时返回 1。
 */
static int
run_selected_tests(char *group_filter, char *name_filter)
{
  int selected = 0;
  int passed = 0;
  int failed;
  int status;

  printf("XV6TEST begin group=%s test=%s\n",
         group_filter == 0 ? "*" : group_filter,
         name_filter == 0 ? "*" : name_filter);

  for(struct xv6_test_case *test = tests; test->name != 0; test++){
    if(!matches_filter(test, group_filter, name_filter))
      continue;
    selected++;
    if(run_test(test, selected))
      passed++;
  }

  failed = selected - passed;
  // 空选择必须失败，否则拼错 group 名时会产生没有执行任何测试的假阳性。
  status = selected == 0 || failed != 0;
  if(selected == 0)
    printf("XV6TEST diagnostic no tests selected\n");
  printf("XV6TEST summary selected=%d passed=%d failed=%d\n",
         selected, passed, failed);
  printf("XV6TEST done status=%d\n", status);
  return status;
}

/**
 * 解析 xv6test 命令行并执行列表、全部、group 或单项测试模式。
 *
 * @param argc 命令行参数数量。
 * @param argv 参数数组；过滤值采用精确匹配，不支持模糊或正则表达式。
 * @return 本函数通过 exit() 返回：测试成功为 0，测试失败为 1，参数错误为 2。
 */
int
main(int argc, char *argv[])
{
  char *group_filter = 0;
  char *name_filter = 0;

  if(argc == 2 && strcmp(argv[1], "--list") == 0){
    exit(list_tests() == 0);
  } else if(argc == 1 || (argc == 2 && strcmp(argv[1], "--all") == 0)){
    // 不带参数与显式 --all 等价，便于在 xv6 shell 中直接运行完整集合。
  } else if(argc == 3 && strcmp(argv[1], "--group") == 0){
    group_filter = argv[2];
  } else if(argc == 3 && strcmp(argv[1], "--run") == 0){
    name_filter = argv[2];
  } else {
    usage(argv[0]);
    exit(2);
  }

  exit(run_selected_tests(group_filter, name_filter));
}
