#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/log.h"
#include "user/user.h"
#include "user/paths.h"

#define LOGCRASH_BYTES (2 * BSIZE)
#define LOGCRASH_OLD   'O'
#define LOGCRASH_NEW   'N'

/**
 * 为下一次文件系统提交武装确定性崩溃点。
 *
 * @param phase kernel/log.h 中的 LOG_CRASH_*；LOG_CRASH_NONE 取消武装。
 * @return 成功返回 0；阶段非法、已有注入点或事务进行中返回 -1。
 */
int logcrash(int phase);

/**
 * 描述一个由 xv6 用户态执行的回归测试。
 *
 * group 用于按 Lab 或能力筛选；name 是稳定且全局唯一的测试名称；argv
 * 是传给 exec() 的空指针结尾参数数组。argv[0] 必须是镜像内绝对路径，避免
 * 测试结果依赖 Shell 当前目录或不存在的 PATH。测试语义由 tests/guest 下的目标
 * 程序拥有，本入口只负责注册、进程隔离、退出状态传播和统一结果协议。
 * orchestrated 测试会改变启动边界或依赖前一次磁盘状态，只允许通过 --run 精确选择。
 */
struct xv6_test_case {
  char *group;
  char *name;
  char **argv;
  int orchestrated;
};

static char logcrash_buffer[LOGCRASH_BYTES];

static char *lab1_sleep_argv[] = {XV6_TEST_PATH("lab1test"), "sleep", 0};
static char *lab1_pingpong_argv[] = {XV6_TEST_PATH("lab1test"), "pingpong", 0};
static char *lab1_primes_argv[] = {XV6_TEST_PATH("lab1test"), "primes", 0};
static char *lab1_find_argv[] = {XV6_TEST_PATH("lab1test"), "find", 0};
static char *lab1_xargs_argv[] = {XV6_TEST_PATH("lab1test"), "xargs", 0};
static char *lab2_tracemask_argv[] = {XV6_TEST_PATH("tracemasktest"), 0};
static char *lab2_sysinfo_argv[] = {XV6_TEST_PATH("sysinfotest"), 0};
static char *lab2_trace_smoke_argv[] = {XV6_TEST_PATH("tracesmoke"), 0};
static char *lab3_copyin_argv[] = {XV6_TEST_PATH("usertests"), "copyin", 0};
static char *lab3_copyout_argv[] = {XV6_TEST_PATH("usertests"), "copyout", 0};
static char *lab3_copyinstr_argv[] = {XV6_TEST_PATH("usertests"), "copyinstr1", 0};
static char *lab3_sbrkmuch_argv[] = {XV6_TEST_PATH("usertests"), "sbrkmuch", 0};
static char *lab3_memviz_argv[] = {XV6_TEST_PATH("memviztest"), 0};
static char *lab3_memtarget_argv[] = {XV6_TEST_PATH("memtargettest"), 0};
static char *lab3_pgtbl_argv[] = {XV6_TEST_PATH("pgtbltest"), 0};
static char *lab3_vaaccess_argv[] = {XV6_TEST_PATH("vaaccesstest"), 0};
static char *lab3_address_window_argv[] = {XV6_TEST_PATH("addresswindowtest"), 0};
static char *lab3_ostep_intro_argv[] = {XV6_TEST_PATH("ostepintrotest"), 0};
static char *lab4_backtrace_argv[] = {XV6_TEST_PATH("bttest"), 0};
static char *lab4_alarm_argv[] = {XV6_TEST_PATH("alarmtest"), 0};
static char *lab5_lazytests_argv[] = {XV6_TEST_PATH("lazytests"), 0};
static char *lab6_cowtest_argv[] = {XV6_TEST_PATH("cowtest"), 0};
static char *lab7_uthread_argv[] = {XV6_TEST_PATH("uthreadtest"), 0};
static char *lab8_kalloc_argv[] = {XV6_TEST_PATH("usertests"), "sbrkmuch", 0};
static char *lab8_createdelete_argv[] = {XV6_TEST_PATH("usertests"), "createdelete", 0};
static char *lab8_fourfiles_argv[] = {XV6_TEST_PATH("usertests"), "fourfiles", 0};
static char *lab8_bigwrite_argv[] = {XV6_TEST_PATH("usertests"), "bigwrite", 0};
static char *lab8_lock_model_argv[] = {XV6_TEST_PATH("locktest"), "positive", 0};
static char *lab9_bigfile_argv[] = {XV6_TEST_PATH("bigfile"), 0};
static char *lab9_symlink_argv[] = {XV6_TEST_PATH("symlinktest"), 0};
static char *largefs_4gib_argv[] = {XV6_TEST_PATH("largefile"), 0};
static char *lab10_mmap_argv[] = {XV6_TEST_PATH("mmaptest"), 0};
static char *core_sbrkbugs_argv[] = {XV6_TEST_PATH("usertests"), "sbrkbugs", 0};
static char *core_forkforkfork_argv[] = {XV6_TEST_PATH("usertests"), "forkforkfork", 0};
static char *core_linkunlink_argv[] = {XV6_TEST_PATH("usertests"), "linkunlink", 0};
static char *core_openiput_argv[] = {XV6_TEST_PATH("usertests"), "openiput", 0};
static char *core_fileapi_argv[] = {XV6_TEST_PATH("fileapitest"), 0};
static char *core_schedtrace_argv[] = {XV6_TEST_PATH("schedtracetest"), 0};
static char *core_disksched_argv[] = {XV6_TEST_PATH("diskschedtest"), 0};
static char *core_history_argv[] = {XV6_TEST_PATH("historytest"), 0};
static char *core_job_control_argv[] = {XV6_TEST_PATH("consolelinetest"), "jobctl", 0};
static char *core_ls_options_argv[] = {XV6_TEST_PATH("lstest"), 0};
static char *core_ps_argv[] = {XV6_TEST_PATH("pstest"), 0};
static char *core_semaphore_argv[] = {XV6_TEST_PATH("semaphoretest"), 0};
static char *legacy_forktest_argv[] = {XV6_TEST_PATH("forktest"), 0};
static char *legacy_stressfs_argv[] = {XV6_TEST_PATH("stressfs"), 0};
static char *legacy_grind_argv[] = {XV6_TEST_PATH("grind"), 0};
static char *full_usertests_argv[] = {XV6_TEST_PATH("usertests"), 0};

static char *logcrash_api_argv[] = {
  XV6_USR_BIN_PATH("xv6test"), "--logcrash-api", 0
};
static char *logcrash_before_prepare_argv[] = {
  XV6_USR_BIN_PATH("xv6test"), "--logcrash-prepare", "before-commit", 0
};
static char *logcrash_before_verify_argv[] = {
  XV6_USR_BIN_PATH("xv6test"), "--logcrash-verify", "before-commit", 0
};
static char *logcrash_after_prepare_argv[] = {
  XV6_USR_BIN_PATH("xv6test"), "--logcrash-prepare", "after-commit", 0
};
static char *logcrash_after_verify_argv[] = {
  XV6_USR_BIN_PATH("xv6test"), "--logcrash-verify", "after-commit", 0
};
static char *logcrash_install_prepare_argv[] = {
  XV6_USR_BIN_PATH("xv6test"), "--logcrash-prepare", "during-install", 0
};
static char *logcrash_install_verify_argv[] = {
  XV6_USR_BIN_PATH("xv6test"), "--logcrash-verify", "during-install", 0
};

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
  {"lab1", "lab1-sleep", lab1_sleep_argv, 0},
  {"lab1", "lab1-pingpong", lab1_pingpong_argv, 0},
  {"lab1", "lab1-primes", lab1_primes_argv, 0},
  {"lab1", "lab1-find", lab1_find_argv, 0},
  {"lab1", "lab1-xargs", lab1_xargs_argv, 0},
  {"lab2", "lab2-tracemask", lab2_tracemask_argv, 0},
  {"lab2", "lab2-sysinfo", lab2_sysinfo_argv, 0},
  {"lab2", "lab2-trace-smoke", lab2_trace_smoke_argv, 0},
  {"lab3", "lab3-copyin", lab3_copyin_argv, 0},
  {"lab3", "lab3-copyout", lab3_copyout_argv, 0},
  {"lab3", "lab3-copyinstr1", lab3_copyinstr_argv, 0},
  {"lab3", "lab3-sbrkmuch", lab3_sbrkmuch_argv, 0},
  {"lab3", "lab3-memviz", lab3_memviz_argv, 0},
  {"lab3", "lab3-memtarget", lab3_memtarget_argv, 0},
  {"lab3", "lab3-pgtbl", lab3_pgtbl_argv, 0},
  {"lab3", "lab3-vaaccess", lab3_vaaccess_argv, 0},
  {"lab3", "lab3-address-window", lab3_address_window_argv, 0},
  {"lab3", "lab3-ostep-intro", lab3_ostep_intro_argv, 0},
  {"lab4", "lab4-backtrace", lab4_backtrace_argv, 0},
  {"lab4", "lab4-alarm", lab4_alarm_argv, 0},
  {"lab5", "lab5-lazytests", lab5_lazytests_argv, 0},
  {"lab6", "lab6-cowtest", lab6_cowtest_argv, 0},
  {"lab7", "lab7-uthread", lab7_uthread_argv, 0},
  {"lab8", "lab8-kalloc-sbrkmuch", lab8_kalloc_argv, 0},
  {"lab8", "lab8-createdelete", lab8_createdelete_argv, 0},
  {"lab8", "lab8-fourfiles", lab8_fourfiles_argv, 0},
  {"lab8", "lab8-bigwrite", lab8_bigwrite_argv, 0},
  {"lab8", "lab8-lock-model", lab8_lock_model_argv, 0},
  {"lab9", "lab9-bigfile", lab9_bigfile_argv, 0},
  {"lab9", "lab9-symlink", lab9_symlink_argv, 0},
  {"largefs", "largefs-4gib", largefs_4gib_argv, 0},
  {"lab10", "lab10-mmap", lab10_mmap_argv, 0},
  {"core", "core-sbrkbugs", core_sbrkbugs_argv, 0},
  {"core", "core-forkforkfork", core_forkforkfork_argv, 0},
  {"core", "core-linkunlink", core_linkunlink_argv, 0},
  {"core", "core-openiput", core_openiput_argv, 0},
  {"core", "core-fileapi", core_fileapi_argv, 0},
  {"core", "core-schedtrace", core_schedtrace_argv, 0},
  {"core", "core-disk-scheduling", core_disksched_argv, 0},
  {"core", "core-shell-history", core_history_argv, 0},
  {"core", "core-job-control", core_job_control_argv, 0},
  {"core", "core-ls-options", core_ls_options_argv, 0},
  {"core", "core-ps", core_ps_argv, 0},
  {"core", "core-semaphore", core_semaphore_argv, 0},
  {"legacy", "legacy-forktest", legacy_forktest_argv, 0},
  {"legacy", "legacy-stressfs", legacy_stressfs_argv, 0},
  {"legacy", "legacy-grind", legacy_grind_argv, 0},
  {"regression", "usertests-full", full_usertests_argv, 0},
  {"logrecovery", "logcrash-api", logcrash_api_argv, 1},
  {"logrecovery", "logcrash-before-prepare", logcrash_before_prepare_argv, 1},
  {"logrecovery", "logcrash-before-verify", logcrash_before_verify_argv, 1},
  {"logrecovery", "logcrash-after-prepare", logcrash_after_prepare_argv, 1},
  {"logrecovery", "logcrash-after-verify", logcrash_after_verify_argv, 1},
  {"logrecovery", "logcrash-install-prepare", logcrash_install_prepare_argv, 1},
  {"logrecovery", "logcrash-install-verify", logcrash_install_verify_argv, 1},
  {0, 0, 0, 0},
};

/** 将实验阶段名称解析为内核故障注入编号。 */
static int
logcrash_phase(char *name)
{
  if(strcmp(name, "before-commit") == 0)
    return LOG_CRASH_BEFORE_COMMIT;
  if(strcmp(name, "after-commit") == 0)
    return LOG_CRASH_AFTER_COMMIT;
  if(strcmp(name, "during-install") == 0)
    return LOG_CRASH_DURING_INSTALL;
  return -1;
}

/** 返回每个实验阶段独占的持久文件路径。 */
static char*
logcrash_path(int phase)
{
  switch(phase){
  case LOG_CRASH_BEFORE_COMMIT:
    return "/log-before";
  case LOG_CRASH_AFTER_COMMIT:
    return "/log-after";
  case LOG_CRASH_DURING_INSTALL:
    return "/log-install";
  default:
    return 0;
  }
}

/** 用稳定字节填充两块事务载荷。 */
static void
logcrash_fill(char value)
{
  memset(logcrash_buffer, value, sizeof(logcrash_buffer));
}

/** 完整写入测试载荷；短写视为失败。 */
static int
logcrash_write_all(int fd)
{
  return write(fd, logcrash_buffer, sizeof(logcrash_buffer)) ==
         sizeof(logcrash_buffer);
}

/** 验证故障注入 API 的参数、占用和取消边界。 */
static int
logcrash_api_test(void)
{
  if(logcrash(-1) != -1 ||
     logcrash(LOG_CRASH_DURING_INSTALL + 1) != -1){
    printf("LOGCRASH api invalid phase accepted\n");
    return 1;
  }
  if(logcrash(LOG_CRASH_BEFORE_COMMIT) != 0){
    printf("LOGCRASH api failed to arm\n");
    return 1;
  }
  if(logcrash(LOG_CRASH_AFTER_COMMIT) != -1){
    printf("LOGCRASH api replaced an armed phase\n");
    return 1;
  }
  if(logcrash(LOG_CRASH_NONE) != 0){
    printf("LOGCRASH api failed to disarm\n");
    return 1;
  }
  printf("LOGCRASH api ok\n");
  return 0;
}

/**
 * 建立旧版本文件，武装指定阶段，并用一个多块事务覆盖为新版本。
 * 正常行为不会从最终 write() 返回；返回即表示预期崩溃没有发生。
 */
static int
logcrash_prepare(int phase)
{
  char *path = logcrash_path(phase);
  int fd;

  if(path == 0)
    return 2;

  unlink(path);
  fd = open(path, O_CREATE | O_TRUNC | O_RDWR);
  if(fd < 0){
    printf("LOGCRASH prepare open-old failed phase=%d\n", phase);
    return 1;
  }
  logcrash_fill(LOGCRASH_OLD);
  if(!logcrash_write_all(fd) || close(fd) < 0){
    printf("LOGCRASH prepare old generation failed phase=%d\n", phase);
    return 1;
  }

  fd = open(path, O_WRONLY);
  if(fd < 0){
    printf("LOGCRASH prepare reopen failed phase=%d\n", phase);
    return 1;
  }
  if(logcrash(phase) < 0){
    close(fd);
    printf("LOGCRASH prepare arm failed phase=%d\n", phase);
    return 1;
  }

  printf("LOGCRASH armed phase=%s bytes=%d\n",
         phase == LOG_CRASH_BEFORE_COMMIT ? "before-commit" :
         phase == LOG_CRASH_AFTER_COMMIT ? "after-commit" : "during-install",
         LOGCRASH_BYTES);
  logcrash_fill(LOGCRASH_NEW);
  if(logcrash_write_all(fd))
    printf("LOGCRASH prepare unexpected write return phase=%d\n", phase);
  else
    printf("LOGCRASH prepare write failed before injection phase=%d\n", phase);
  logcrash(LOG_CRASH_NONE);
  close(fd);
  return 1;
}

/**
 * 从重启后的真实磁盘镜像读取文件，断言事务只呈现完整旧版本或完整新版本。
 */
static int
logcrash_verify(int phase)
{
  char *path = logcrash_path(phase);
  char expected = phase == LOG_CRASH_BEFORE_COMMIT ? LOGCRASH_OLD : LOGCRASH_NEW;
  struct stat st;
  int old_count = 0;
  int new_count = 0;
  int other_count = 0;
  int total = 0;
  int fd;
  char extra;

  if(path == 0)
    return 2;
  fd = open(path, O_RDONLY);
  if(fd < 0){
    printf("LOGCRASH verify open failed phase=%d\n", phase);
    return 1;
  }
  if(fstat(fd, &st) < 0 || st.size != LOGCRASH_BYTES){
    printf("LOGCRASH verify size failed phase=%d size=%d\n", phase, st.size);
    close(fd);
    return 1;
  }

  while(total < LOGCRASH_BYTES){
    int n = read(fd, logcrash_buffer + total, LOGCRASH_BYTES - total);
    if(n <= 0){
      printf("LOGCRASH verify short read phase=%d total=%d\n", phase, total);
      close(fd);
      return 1;
    }
    total += n;
  }
  if(read(fd, &extra, 1) != 0){
    printf("LOGCRASH verify trailing data phase=%d\n", phase);
    close(fd);
    return 1;
  }
  close(fd);

  for(int i = 0; i < LOGCRASH_BYTES; i++){
    if(logcrash_buffer[i] == LOGCRASH_OLD)
      old_count++;
    else if(logcrash_buffer[i] == LOGCRASH_NEW)
      new_count++;
    else
      other_count++;
  }

  printf("LOGCRASH verify phase=%d expected=%c old=%d new=%d other=%d\n",
         phase, expected, old_count, new_count, other_count);
  if(other_count != 0)
    return 1;
  if(expected == LOGCRASH_OLD)
    return old_count == LOGCRASH_BYTES && new_count == 0 ? 0 : 1;
  return new_count == LOGCRASH_BYTES && old_count == 0 ? 0 : 1;
}

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
    printf("XV6TEST case name=%s group=%s command=%s orchestrated=%d\n",
           test->name, test->group, test->argv[0], test->orchestrated);
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
    if(test->orchestrated && name_filter == 0)
      continue;
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
  char *argv[] = {XV6_TEST_PATH("usertests"), name, 0};
  struct xv6_test_case test = {"regression", name, argv, 0};
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

  if(argc == 2 && strcmp(argv[1], "--logcrash-api") == 0){
    exit(logcrash_api_test());
  } else if(argc == 3 && strcmp(argv[1], "--logcrash-prepare") == 0){
    int phase = logcrash_phase(argv[2]);
    exit(phase < 0 ? 2 : logcrash_prepare(phase));
  } else if(argc == 3 && strcmp(argv[1], "--logcrash-verify") == 0){
    int phase = logcrash_phase(argv[2]);
    exit(phase < 0 ? 2 : logcrash_verify(phase));
  } else if(argc == 2 && strcmp(argv[1], "--list") == 0){
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
