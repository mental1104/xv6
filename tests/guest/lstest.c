#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define OUTPUT_SIZE 2048

// 专项输出包含 ANSI 颜色和长格式字段；放在 BSS 避免占满 xv6 单页用户栈。
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
 * 判断字符是否为十进制数字。
 *
 * @param value 待判断字符。
 * @return 数字返回 1，否则返回 0。
 */
static int
is_digit(char value)
{
  return value >= '0' && value <= '9';
}

/**
 * 判断长格式输出中是否存在 `Mon DD HH:MM` 形式的 UTC 时间字段。
 *
 * @param text ls 输出。
 * @return 找到合法固定宽度字段返回 1，否则返回 0。
 */
static int
has_mtime_field(char *text)
{
  static char *months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  int i;
  int month;

  for(i = 0; text[i] != 0; i++){
    if(text[i + 11] == 0)
      break;
    for(month = 0; month < 12; month++){
      if(memcmp(text + i, months[month], 3) != 0)
        continue;
      if(text[i + 3] == ' ' &&
         (text[i + 4] == ' ' || is_digit(text[i + 4])) &&
         is_digit(text[i + 5]) && text[i + 6] == ' ' &&
         is_digit(text[i + 7]) && is_digit(text[i + 8]) &&
         text[i + 9] == ':' && is_digit(text[i + 10]) &&
         is_digit(text[i + 11]))
        return 1;
    }
  }
  return 0;
}

/**
 * 在子进程中执行 ls，并同时捕获 stdout 与 stderr。
 *
 * @param argv 传给 exec("ls") 的空指针结尾参数数组。
 * @param buffer 输出缓冲区。
 * @param capacity buffer 容量，必须大于 1。
 * @return ls 的退出状态；基础设施失败时直接终止测试。
 */
static int
run_ls(char **argv, char *buffer, int capacity)
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
    count = read(pipefd[0], buffer + total, capacity - total - 1);
    if(count <= 0)
      break;
    total += count;
  }
  buffer[total] = 0;
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

/**
 * 验证默认横向布局、长格式、颜色、total、组合选项和错误状态传播。
 */
static void
test_ls_options(void)
{
  char *default_argv[] = {"ls", "lstmp", 0};
  char *all_argv[] = {"ls", "-a", "lstmp", 0};
  char *long_argv[] = {"ls", "-l", "lstmp", 0};
  char *human_argv[] = {"ls", "-lh", "lstmp", 0};
  char *combined_argv[] = {"ls", "-alh", "lstmp", 0};
  char *device_argv[] = {"ls", "-l", "console", 0};
  char *dash_argv[] = {"ls", "--", "-lsdash", 0};
  char *help_argv[] = {"ls", "--help", 0};
  char *invalid_argv[] = {"ls", "-z", 0};
  char *multi_argv[] = {"ls", "missing-ls", "lstmp/visible", 0};
  char *default_expected = "\033[1;34msubdir\033[0m visible link\n";
  char *all_expected = "\033[1;34m.\033[0m \033[1;34m..\033[0m \033[1;34msubdir\033[0m visible .hidden link\n";

  check(run_ls(default_argv, output, sizeof(output)) == 0, "default ls status");
  check(strcmp(output, default_expected) == 0, "default ls is not one colored line");

  check(run_ls(all_argv, output, sizeof(output)) == 0, "ls -a status");
  check(strcmp(output, all_expected) == 0, "ls -a layout or hidden entries");

  check(run_ls(long_argv, output, sizeof(output)) == 0, "ls -l status");
  check(contains(output, "total "), "ls -l missing total");
  check(contains(output, "-rw-r--r--"), "ls -l missing file mode");
  check(contains(output, "drwxr-xr-x"), "ls -l missing directory mode");
  check(contains(output, "lrwxrwxrwx"), "ls -l missing symlink mode");
  check(contains(output, " root root "), "ls -l missing owner or group");
  check(contains(output, "1536 "), "ls -l missing byte size");
  check(contains(output, "\033[1;34msubdir\033[0m"), "ls -l missing directory color");
  check(has_mtime_field(output), "ls -l missing modification time");

  check(run_ls(human_argv, output, sizeof(output)) == 0, "ls -lh status");
  check(contains(output, "total 1.5K\n"), "ls -lh missing human total");
  check(contains(output, "1.5K "), "ls -lh missing human file size");

  check(run_ls(combined_argv, output, sizeof(output)) == 0, "ls -alh status");
  check(contains(output, ".hidden"), "ls -alh missing hidden file");
  check(contains(output, "1.5K "), "ls -alh lost human size");

  check(run_ls(device_argv, output, sizeof(output)) == 0, "ls device status");
  check(contains(output, "crw-rw-rw-"), "ls missing device mode");
  check(contains(output, " root root "), "ls device missing owner or group");

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

/** 验证 mkfs 初始时间和运行期写入、重开、截断的 mtime 持久化。 */
static void
test_mtime(void)
{
  int fd;
  struct stat initial;
  struct stat before;
  struct stat after_write;
  struct stat reopened;
  struct stat truncated;

  fd = open("README", O_RDONLY);
  check(fd >= 0, "open README failed");
  check(fstat(fd, &initial) == 0, "stat README failed");
  check(initial.mtime > 0, "mkfs inode mtime is zero");
  close(fd);

  fd = open("lstmp/visible", O_RDWR);
  check(fd >= 0, "open visible for mtime failed");
  check(fstat(fd, &before) == 0, "stat before write failed");
  check(write(fd, "y", 1) == 1, "mtime write failed");
  check(fstat(fd, &after_write) == 0, "stat after write failed");
  check(after_write.mtime >= before.mtime, "write moved mtime backwards");
  close(fd);

  fd = open("lstmp/visible", O_RDONLY);
  check(fd >= 0, "reopen visible failed");
  check(fstat(fd, &reopened) == 0, "stat reopened file failed");
  check(reopened.mtime == after_write.mtime, "mtime was not persisted");
  close(fd);

  fd = open("lstmp/visible", O_RDWR | O_TRUNC);
  check(fd >= 0, "truncate visible failed");
  check(fstat(fd, &truncated) == 0, "stat truncated file failed");
  check(truncated.size == 0, "truncate did not clear size");
  check(truncated.mtime >= reopened.mtime, "truncate moved mtime backwards");
  close(fd);
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
  test_mtime();
  remove_fixture();
  printf("lstest: OK\n");
  exit(0);
}
