#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define PRIMARY_PATH "fa_primary"
#define ALIAS_PATH "fa_alias"

/**
 * 立即报告断言失败并以非零状态终止测试。
 *
 * @param reason 可直接定位失败契约的稳定说明文本。
 */
static void
fail(const char *reason)
{
  printf("fileapitest: %s\n", reason);
  exit(1);
}

/**
 * 关闭测试持有的文件描述符，并把关闭失败视为资源回收失败。
 *
 * @param fd 当前测试独占持有的有效文件描述符。
 * @param reason close() 失败时输出的定位文本。
 */
static void
close_checked(int fd, const char *reason)
{
  if(close(fd) < 0)
    fail(reason);
}

/**
 * 从当前偏移读取一个字节并校验内容。
 *
 * @param fd 可读文件描述符；调用后其所属打开文件对象偏移推进一字节。
 * @param expected 期望读到的单字节内容。
 * @param reason 读取长度或内容不符合契约时输出的定位文本。
 */
static void
expect_byte(int fd, char expected, const char *reason)
{
  char actual = 0;

  if(read(fd, &actual, 1) != 1 || actual != expected)
    fail(reason);
}

/** 清除上次运行可能留下的目录项，使测试可在同一 guest 中重复执行。 */
static void
cleanup_workspace(void)
{
  unlink(PRIMARY_PATH);
  unlink(ALIAS_PATH);
}

/**
 * 验证独立 open 拥有独立偏移，而 dup 与 fork 共享同一个打开文件对象偏移。
 *
 * 该测试同时验证失败的 lseek 不得修改原偏移，避免错误路径破坏后续读取位置。
 */
static void
test_offset_ownership(void)
{
  int fd;
  int independent;
  int shared;
  int pid;
  int status = -1;

  fd = open(PRIMARY_PATH, O_CREATE | O_TRUNC | O_RDWR);
  if(fd < 0)
    fail("cannot create primary file");
  if(write(fd, "ABCDE", 5) != 5)
    fail("initial write did not complete");
  if(lseek(fd, 0, SEEK_SET) != 0)
    fail("cannot rewind primary descriptor");

  independent = open(PRIMARY_PATH, O_RDONLY);
  if(independent < 0)
    fail("cannot independently reopen primary file");

  expect_byte(fd, 'A', "first open did not read A");
  expect_byte(independent, 'A', "independent open unexpectedly shared offset");

  shared = dup(fd);
  if(shared < 0)
    fail("dup failed");
  expect_byte(shared, 'B', "dup did not inherit shared offset");
  expect_byte(fd, 'C', "original descriptor did not observe dup offset advance");

  if(lseek(fd, 0, SEEK_SET) != 0)
    fail("cannot prepare fork offset test");
  pid = fork();
  if(pid < 0)
    fail("fork failed");
  if(pid == 0){
    expect_byte(fd, 'A', "child did not read from inherited offset");
    exit(0);
  }
  if(wait(&status) != pid || status != 0)
    fail("child offset observation failed");
  expect_byte(fd, 'B', "parent did not observe child offset advance");

  if(lseek(fd, 4, SEEK_SET) != 4)
    fail("cannot position before final byte");
  if(lseek(fd, -5, SEEK_CUR) != -1)
    fail("negative seek before file start was accepted");
  if(lseek(fd, 0, SEEK_CUR) != 4)
    fail("failed seek changed the shared offset");
  expect_byte(fd, 'E', "failed seek corrupted the next read position");

  close_checked(shared, "cannot close duplicated descriptor");
  close_checked(independent, "cannot close independent descriptor");
  close_checked(fd, "cannot close primary descriptor");
}

/**
 * 验证目录项、inode 链接计数与打开引用拥有彼此独立的生命周期。
 *
 * 最后一个名字删除后，既有 fd 仍须访问旧 inode；在旧 fd 关闭前以同名创建的新文件
 * 必须获得另一 inode，从而击穿“unlink 会立即销毁已打开文件”的错误直觉。
 */
static void
test_link_unlink_lifetime(void)
{
  struct stat original;
  struct stat linked;
  struct stat after_failure;
  struct stat after_unlink;
  struct stat replacement;
  int retained;
  int alias_fd;
  int replacement_fd;
  int unexpected;
  char old_data[5];

  retained = open(PRIMARY_PATH, O_RDONLY);
  if(retained < 0 || fstat(retained, &original) < 0)
    fail("cannot inspect original inode");

  if(link(PRIMARY_PATH, ALIAS_PATH) < 0)
    fail("cannot create hard link");
  alias_fd = open(ALIAS_PATH, O_RDONLY);
  if(alias_fd < 0 || fstat(alias_fd, &linked) < 0)
    fail("cannot inspect linked inode");
  if(original.dev != linked.dev || original.ino != linked.ino ||
     linked.nlink != 2)
    fail("hard link did not reference the same inode");
  close_checked(alias_fd, "cannot close hard-link descriptor");

  if(link(PRIMARY_PATH, ALIAS_PATH) != -1)
    fail("link unexpectedly replaced an existing directory entry");
  if(fstat(retained, &after_failure) < 0 || after_failure.nlink != 2)
    fail("failed link did not roll back inode link count");

  if(unlink(PRIMARY_PATH) < 0)
    fail("cannot unlink primary directory entry");
  unexpected = open(PRIMARY_PATH, O_RDONLY);
  if(unexpected >= 0){
    close(unexpected);
    fail("removed primary path remained resolvable");
  }
  if(fstat(retained, &after_unlink) < 0 || after_unlink.nlink != 1)
    fail("first unlink did not decrement link count");

  if(unlink(ALIAS_PATH) < 0)
    fail("cannot unlink final directory entry");
  unexpected = open(ALIAS_PATH, O_RDONLY);
  if(unexpected >= 0){
    close(unexpected);
    fail("removed alias path remained resolvable");
  }
  if(fstat(retained, &after_unlink) < 0 || after_unlink.nlink != 0)
    fail("final unlink did not expose zero link count on open inode");
  if(lseek(retained, 0, SEEK_SET) != 0 ||
     read(retained, old_data, sizeof(old_data)) != sizeof(old_data) ||
     memcmp(old_data, "ABCDE", sizeof(old_data)) != 0)
    fail("open descriptor lost data after final unlink");

  replacement_fd = open(ALIAS_PATH, O_CREATE | O_TRUNC | O_RDWR);
  if(replacement_fd < 0 || write(replacement_fd, "Z", 1) != 1 ||
     fstat(replacement_fd, &replacement) < 0)
    fail("cannot create replacement file");
  if(replacement.dev == original.dev && replacement.ino == original.ino)
    fail("replacement reused inode before final open reference closed");

  if(lseek(retained, 0, SEEK_SET) != 0)
    fail("cannot rewind unlinked open inode");
  expect_byte(retained, 'A', "replacement changed retained inode data");
  if(lseek(replacement_fd, 0, SEEK_SET) != 0)
    fail("cannot rewind replacement file");
  expect_byte(replacement_fd, 'Z', "replacement file content is incorrect");

  close_checked(retained, "cannot release final old-inode reference");
  close_checked(replacement_fd, "cannot close replacement file");
  if(unlink(ALIAS_PATH) < 0)
    fail("cannot remove replacement file");

  if(unlink(PRIMARY_PATH) != -1 || link(PRIMARY_PATH, ALIAS_PATH) != -1)
    fail("missing-path operations unexpectedly succeeded");
}

/**
 * 构造文件 API 的状态闭环并通过退出状态向 xv6test 报告结果。
 *
 * @return 全部状态、错误和资源回收断言通过时以 exit(0) 终止；失败路径由 fail()
 *         以 exit(1) 终止。
 */
int
main(void)
{
  cleanup_workspace();
  test_offset_ownership();
  test_link_unlink_lifetime();
  cleanup_workspace();

  printf("fileapitest: verified offsets, links, unlink lifetime, and cleanup\n");
  exit(0);
}
