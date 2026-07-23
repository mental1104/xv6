#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define OUTPUT_SIZE 1024

// 1024 字节足以覆盖专项输出；放在 BSS 避免占满 xv6 的单页用户栈。
static char output[OUTPUT_SIZE];

/**
 * 在断言失败时打印稳定诊断并终止测试。
 *
 * @param condition 非零表示断言成立。
 * @param message 失败时输出的场景说明。
 */
static void
check(int condition, char *message)
{
  if(condition)
    return;
  printf("lstest: %s\n", message);
  exit(1);
}

/**
 * 判断完整输出中是否包含指定连续文本。
 *
 * @param text 待搜索输出。
 * @param pattern 需要出现的子串。
 * @return 找到返回 1，否则返回 0。
 */
static int
contains(char *text, char *pattern)
{
  int i;
  int j;

  if(pattern[0] == 0)
    return 1;
  for(i = 0; text[i] != 0; i++){
    for(j = 0; pattern[j] != 0 && text[i + j] == pattern[j]; j++)
      ;
    if(pattern[j] == 0)
      return 1;
  }
  return 0;
}

/**
 * 判断输出中是否存在内容完全相同的一整行。
 *
 * @param text 多行输出。
 * @param expected 目标行，不包含换行符。
 * @return 找到完整行返回 1，否则返回 0。
 */
static int
has_line(char *text, char *expected)
{
  int start;
  int end;
  int expected_length;

  expected_length = strlen(expected);
  start = 0;
  while(text[start] != 0){
    end = start;
    while(text[end] != 0 && text[end] != '\n')
      end++;
    if(end - start == expected_length && memcmp(text + start, expected, expected_length) == 0)
      return 1;
    start = text[end] == '\n' ? end + 1 : end;
  }
  return 0;
}

/**
 * 在子进程中执行 ls，并同时捕获 stdout 与 stderr。
 *
 * @param argv 传给 exec("ls") 的空指针结尾参数数组。
 * @param output 输出缓冲区。
 * @param capacity output 容量，必须大于 1。
 * @return ls 的退出状态；基础设施失败时直接终止测试。
 */
static int
run_ls(char **argv, char *output, int capacity)
{
  int pipefd[2];
  int pid;
  int status;
  int total;
  int count;

  check(pipe(pipefd) == 0, "pipe failed");
  pid = fork();
  check(pid >= 0, "fork failed");
  if(pid == 0){
    close(pipefd[0]);
    close(1);
    check(dup(pipefd[1]) == 1, "redirect stdout failed");
    close(2);
    check(dup(pipefd[1]) == 2, "redirect stderr failed");
    close(pipefd[1]);
    exec("ls", argv);
    exit(127);
  }

  close(pipefd[1]);
  total = 0;
  while(total < capacity - 1){
    count = read(pipefd[0], output + total, capacity - total - 1);
    if(count <= 0)
      break;
    total += count;
  }
  output[total] = 0;
  close(pipefd[0]);
  check(wait(&status) == pid, "wait returned wrong child");
  return status;
}

/** 创建 ls 选项测试使用的目录、隐藏文件、普通文件和符号链接。 */
static void
create_fixture(void)
{
  char block[512];
  int fd;
  int i;

  unlink("lstmp/link");
  unlink("lstmp/.hidden");
  unlink("lstmp/visible");
  unlink("lstmp/subdir");
  unlink("lstmp");
  unlink("-lsdash");

  check(mkdir("lstmp") == 0, "mkdir fixture failed");
  check(mkdir("lstmp/subdir") == 0, "mkdir subdir failed");
  memset(block, 'x', sizeof(block));
  fd = open("lstmp/visible", O_CREATE | O_RDWR | O_TRUNC);
  check(fd >= 0, "open visible failed");
  for(i = 0; i < 3; i++)
    check(write(fd, block, sizeof(block)) == sizeof(block), "write visible failed");
  close(fd);

  fd = open("lstmp/.hidden", O_CREATE | O_WRONLY | O_TRUNC);
  check(fd >= 0, "open hidden failed");
  check(write(fd, "h", 1) == 1, "write hidden failed");
  close(fd);
  check(symlink("visible", "lstmp/link") == 0, "create symlink failed");

  fd = open("-lsdash", O_CREATE | O_WRONLY | O_TRUNC);
  check(fd >= 0, "open dash path failed");
  close(fd);
}

/** 删除 fixture，确保重复执行不会因残留目录项失败。 */
static void
remove_fixture(void)
{
  unlink("lstmp/link");
  unlink("lstmp/.hidden");
  unlink("lstmp/visible");
  unlink("lstmp/subdir");
  unlink("lstmp");
  unlink("-lsdash");
}

/** 验证默认隐藏、-a、-l、-h、组合选项、-- 和错误状态传播。 */
static void
test_ls_options(void)
{
  char *default_argv[] = {"ls", "lstmp", 0};
  char *all_argv[] = {"ls", "-a", "lstmp", 0};
  char *long_argv[] = {"ls", "-l", "lstmp", 0};
  char *human_argv[] = {"ls", "-lh", "lstmp", 0};
  char *combined_argv[] = {"ls", "-alh", "lstmp", 0};
  char *dash_argv[] = {"ls", "--", "-lsdash", 0};
  char *help_argv[] = {"ls", "--help", 0};
  char *invalid_argv[] = {"ls", "-z", 0};
  char *multi_argv[] = {"ls", "missing-ls", "lstmp/visible", 0};

  check(run_ls(default_argv, output, sizeof(output)) == 0, "default ls status");
  check(has_line(output, "visible"), "default ls missing visible");
  check(has_line(output, "subdir"), "default ls missing directory");
  check(has_line(output, "link"), "default ls missing symlink");
  check(!has_line(output, "."), "default ls exposed dot");
  check(!has_line(output, ".."), "default ls exposed dotdot");
  check(!has_line(output, ".hidden"), "default ls exposed hidden file");

  check(run_ls(all_argv, output, sizeof(output)) == 0, "ls -a status");
  check(has_line(output, "."), "ls -a missing dot");
  check(has_line(output, ".."), "ls -a missing dotdot");
  check(has_line(output, ".hidden"), "ls -a missing hidden file");

  check(run_ls(long_argv, output, sizeof(output)) == 0, "ls -l status");
  check(contains(output, "1536 visible"), "ls -l missing byte size");
  check(contains(output, "d "), "ls -l missing directory type");
  check(contains(output, "l "), "ls -l missing symlink type");

  check(run_ls(human_argv, output, sizeof(output)) == 0, "ls -lh status");
  check(contains(output, "1.5K visible"), "ls -lh missing human size");

  check(run_ls(combined_argv, output, sizeof(output)) == 0, "ls -alh status");
  check(contains(output, ".hidden"), "ls -alh missing hidden file");
  check(contains(output, "1.5K visible"), "ls -alh lost human size");

  check(run_ls(dash_argv, output, sizeof(output)) == 0, "ls -- status");
  check(has_line(output, "-lsdash"), "ls -- did not preserve dash path");

  check(run_ls(help_argv, output, sizeof(output)) == 0, "ls --help status");
  check(contains(output, "Usage: ls [-alh]"), "ls --help missing usage");

  check(run_ls(invalid_argv, output, sizeof(output)) == 2, "invalid option status");
  check(contains(output, "invalid option"), "invalid option diagnostic");
  check(contains(output, "Usage: ls"), "invalid option usage");

  check(run_ls(multi_argv, output, sizeof(output)) == 1, "multi-path failure status");
  check(contains(output, "cannot access 'missing-ls'"), "missing path diagnostic");
  check(has_line(output, "visible"), "multi-path stopped before valid path");
}

/**
 * 创建 fixture、执行全部断言并清理。
 *
 * @return 通过 exit status 返回；成功为 0，任一断言失败为 1。
 */
int
main(void)
{
  create_fixture();
  test_ls_options();
  remove_fixture();
  printf("lstest: OK\n");
  exit(0);
}
