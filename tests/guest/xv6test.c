#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/**
 * 描述一个由 xv6 用户态执行的回归测试。
 *
 * group 用于按 Lab 或能力筛选；name 是稳定且全局唯一的测试名称；argv
 * 是传给 exec() 的空指针结尾参数数组。测试语义由 tests/guest 下的目标
 * 程序拥有，本入口只负责注册、进程隔离、退出状态传播和统一结果协议。
 */
struct xv6_test_case {
  char *group;
  char *name;
  char **argv;
};

static char *lab1_sleep_argv[] = {"lab1test", "sleep", 0};
static char *lab1_pingpong_argv[] = {"lab1test", "pingpong", 0};
static char *lab1_primes_argv[] = {"lab1test", "primes", 0};
static char *lab1_find_argv[] = {"lab1test", "find", 0};
static char *lab1_xargs_argv[] = {"lab1test", "xargs", 0};
static char *lab2_tracemask_argv[] = {"tracemasktest", 0};
static char *lab2_sysinfo_argv[] = {"sysinfotest", 0};
static char *lab2_trace_smoke_argv[] = {"tracesmoke", 0};
static char *lab3_copyin_argv[] = {"usertests", "copyin", 0};
static char *lab3_copyout_argv[] = {"usertests", "copyout", 0};
static char *lab3_copyinstr_argv[] = {"usertests", "copyinstr1", 0};
static char *lab3_sbrkmuch_argv[] = {"usertests", "sbrkmuch", 0};
static char *lab3_memviz_argv[] = {"memviztest", 0};
static char *lab3_vaaccess_argv[] = {"vaaccesstest", 0};
static char *lab3_address_window_argv[] = {"addresswindowtest", 0};
static char *lab4_backtrace_argv[] = {"bttest", 0};
static char *lab4_alarm_argv[] = {"alarmtest", 0};
static char *lab5_lazytests_argv[] = {"lazytests", 0};
static char *lab6_cowtest_argv[] = {"cowtest", 0};
static char *lab7_uthread_argv[] = {"uthreadtest", 0};
static char *lab8_kalloc_argv[] = {"usertests", "sbrkmuch", 0};
static char *lab8_createdelete_argv[] = {"usertests", "createdelete", 0};
static char *lab8_fourfiles_argv[] = {"usertests", "fourfiles", 0};
static char *lab8_bigwrite_argv[] = {"usertests", "bigwrite", 0};
static char *lab9_bigfile_argv[] = {"bigfile", 0};
static char *lab9_symlink_argv[] = {"symlinktest", 0};
static char *largefs_4gib_argv[] = {"largefile", 0};
static char *lab10_mmap_argv[] = {"mmaptest", 0};
static char *core_sbrkbugs_argv[] = {"usertests", "sbrkbugs", 0};
static char *core_forkforkfork_argv[] = {"usertests", "forkforkfork", 0};
static char *core_linkunlink_argv[] = {"usertests", "linkunlink", 0};
static char *core_openiput_argv[] = {"usertests", "openiput", 0};
static char *core_schedtrace_argv[] = {"schedtracetest", 0};
static char *core_history_argv[] = {"historytest", 0};
static char *core_ls_options_argv[] = {"lstest", 0};
static char *legacy_forktest_argv[] = {"forktest", 0};
static char *legacy_stressfs_argv[] = {"stressfs", 0};
static char *legacy_grind_argv[] = {"grind", 0};
static char *full_usertests_argv[] = {"usertests", 0};

// usertests 对未知名称会执行零项后成功退出，因此动态入口必须先做白名单校验。
static char *usertest_names[] = {
  "execout", "copyin", "copyout", "copyinstr1", "copyinstr2", "copyinstr3",
  "truncate1", "truncate2", "truncate3", "reparent2", "jobctl", "pgbug",
  "sbrkbugs", "badarg", "reparent", "twochildren", "forkfork",
  "forkforkfork", "argptest", "createdelete", "linkunlink", "linktest",
  "unlinkread", "concreate", "subdir", "fourfiles", "sharedfd", "exectest",
  "bigargtest", "bigwrite", "bsstest", "sbrkbasic", "sbrkmuch", "kernmem",
  "sbrkfail", "sbrkarg", "validatetest", "stacktest", "opentest", "writetest",
  "writebig", "createtest", "openiput", "exitiput", "iput", "mem", "pipe1",
  "preempt", "exitwait", "rmdot", "fourteen", "bigfile", "dirfile", "iref",
  "forktest", "bigdir", 0,
};

static struct xv6_test_case tests[] = {
  {"lab1", "lab1-sleep", lab1_sleep_argv},
  {"lab1", "lab1-pingpong", lab1_pingpong_argv},
  {"lab1", "lab1-primes", lab1_primes_argv},
  {"lab1", "lab1-find", lab1_find_argv},
  {"lab1", "lab1-xargs", lab1_xargs_argv},
  {"lab2", "lab2-tracemask", lab2_tracemask_argv},
  {"lab2", "lab2-sysinfo", lab2_sysinfo_argv},
  {"lab2", "lab2-trace-smoke", lab2_trace_smoke_argv},
  {"lab3", "lab3-copyin", lab3_copyin_argv},
  {"lab3", "lab3-copyout", lab3_copyout_argv},
  {"lab3", "lab3-copyinstr1", lab3_copyinstr_argv},
  {"lab3", "lab3-sbrkmuch", lab3_sbrkmuch_argv},
  {"lab3", "lab3-memviz", lab3_memviz_argv},
  {"lab3", "lab3-vaaccess", lab3_vaaccess_argv},
  {"lab3", "lab3-address-window", lab3_address_window_argv},
  {"lab4", "lab4-backtrace", lab4_backtrace_argv},
  {"lab4", "lab4-alarm", lab4_alarm_argv},
  {"lab5", "lab5-lazytests", lab5_lazytests_argv},
  {"lab6", "lab6-cowtest", lab6_cowtest_argv},
  {"lab7", "lab7-uthread", lab7_uthread_argv},
  {"lab8", "lab8-kalloc-sbrkmuch", lab8_kalloc_argv},
  {"lab8", "lab8-createdelete", lab8_createdelete_argv},
  {"lab8", "lab8-fourfiles", lab8_fourfiles_argv},
  {"lab8", "lab8-bigwrite", lab8_bigwrite_argv},
  {"lab9", "lab9-bigfile", lab9_bigfile_argv},
  {"lab9", "lab9-symlink", lab9_symlink_argv},
  {"largefs", "largefs-4gib", largefs_4gib_argv},
  {"lab10", "lab10-mmap", lab10_mmap_argv},
  {"core", "core-sbrkbugs", core_sbrkbugs_argv},
  {"core", "core-forkforkfork", core_forkforkfork_argv},
  {"core", "core-linkunlink", core_linkunlink_argv},
  {"core", "core-openiput", core_openiput_argv},
  {"core", "core-schedtrace", core_schedtrace_argv},
  {"core", "core-shell-history", core_history_argv},
  {"core", "core-ls-options", core_ls_options_argv},
  {"legacy", "legacy-forktest", legacy_forktest_argv},
  {"legacy", "legacy-stressfs", legacy_stressfs_argv},
  {"legacy", "legacy-grind", legacy_grind_argv},
  {"regression", "usertests-full", full_usertests_argv},
  {0, 0, 0},
};

static void
usage(char *program)
{
  fprintf(2,
          "Usage: %s [--all | --list | --group name | --run test | --usertest name]\n",
          program);
}

static int
matches_filter(struct xv6_test_case *test, char *group_filter, char *name_filter)
{
  if(group_filter != 0 && strcmp(test->group, group_filter) != 0)
    return 0;
  if(name_filter != 0 && strcmp(test->name, name_filter) != 0)
    return 0;
  return 1;
}

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
    exec(test->argv[0], test->argv);
    printf("XV6TEST diagnostic name=%s exec=%s failed\n",
           test->name, test->argv[0]);
    exit(1);
  }
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
  status = selected == 0 || failed != 0;
  if(selected == 0)
    printf("XV6TEST diagnostic no tests selected\n");
  printf("XV6TEST summary selected=%d passed=%d failed=%d\n",
         selected, passed, failed);
  printf("XV6TEST done status=%d\n", status);
  return status;
}

/** 判断动态 usertests 名称是否存在于当前镜像支持的静态列表。 */
static int
known_usertest(char *name)
{
  for(char **candidate = usertest_names; *candidate != 0; candidate++)
    if(strcmp(*candidate, name) == 0)
      return 1;
  return 0;
}

/** 通过统一 XV6TEST 协议运行一个动态选择的 usertests 子项。 */
static int
run_usertest(char *name)
{
  char *argv[] = {"usertests", name, 0};
  struct xv6_test_case test = {"regression", name, argv};
  int passed;
  int status;

  printf("XV6TEST begin group=regression test=%s\n", name);
  passed = run_test(&test, 1);
  status = !passed;
  printf("XV6TEST summary selected=1 passed=%d failed=%d\n",
         passed, status);
  printf("XV6TEST done status=%d\n", status);
  return status;
}

int
main(int argc, char *argv[])
{
  char *group_filter = 0;
  char *name_filter = 0;

  if(argc == 2 && strcmp(argv[1], "--list") == 0){
    exit(list_tests() == 0);
  } else if(argc == 1 || (argc == 2 && strcmp(argv[1], "--all") == 0)){
  } else if(argc == 3 && strcmp(argv[1], "--group") == 0){
    group_filter = argv[2];
  } else if(argc == 3 && strcmp(argv[1], "--run") == 0){
    name_filter = argv[2];
  } else if(argc == 3 && strcmp(argv[1], "--usertest") == 0){
    if(!known_usertest(argv[2])){
      printf("XV6TEST diagnostic unknown usertest=%s\n", argv[2]);
      printf("XV6TEST summary selected=0 passed=0 failed=0\n");
      printf("XV6TEST done status=1\n");
      exit(1);
    }
    exit(run_usertest(argv[2]));
  } else {
    usage(argv[0]);
    exit(2);
  }

  exit(run_selected_tests(group_filter, name_filter));
}
