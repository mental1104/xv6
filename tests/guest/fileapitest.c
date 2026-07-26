#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fsinspect.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define PRIMARY_PATH "fa_primary"
#define ALIAS_PATH "fa_alias"
#define FSWALK_DIR "/tmp/fswalk"
#define FSWALK_PATH "/tmp/fswalk/data"
#define FSWALK_BLOCKS (NDIRECT + 1)

static int fswalk_fd = -1;
static char fswalk_block[BSIZE];

/** 清理文件系统 Golden Trace 持有的描述符与目录项。 */
static void
cleanup_fswalk(void)
{
  if(fswalk_fd >= 0){
    close(fswalk_fd);
    fswalk_fd = -1;
  }
  unlink(FSWALK_PATH);
  unlink(FSWALK_DIR);
}

/**
 * 立即报告断言失败并以非零状态终止测试。
 *
 * @param reason 可直接定位失败契约的稳定说明文本。
 */
static void
fail(const char *reason)
{
  cleanup_fswalk();
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
  cleanup_fswalk();
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

/** 读取一个全局或 fd 关联的文件系统观察结果。 */
static void
fswalk_snapshot(int fd, struct fsinspect_snapshot *out)
{
  if(fsinspect(fd, out) < 0)
    fail("fsinspect failed");
  if(out->version != FSINSPECT_VERSION)
    fail("unexpected fsinspect version");
}

/** 用逻辑块编号派生出的固定字节填满一个数据块。 */
static void
fswalk_fill_block(int logical_block)
{
  char value = 'A' + logical_block;

  for(int i = 0; i < BSIZE; i++)
    fswalk_block[i] = value;
}

/** 验证读回块与写入时的固定字节完全一致。 */
static void
fswalk_check_block(int logical_block)
{
  char expected = 'A' + logical_block;

  for(int i = 0; i < BSIZE; i++)
    if(fswalk_block[i] != expected)
      fail("filesystem read data mismatch");
}

/**
 * 验证目录、inode、块映射、位图、日志和缓存围绕同一文件生命周期协同变化。
 *
 * 观察接口只读且不冻结其他 CPU；本测试通过独占路径和无并发写入者比较前后增量。
 */
static void
test_filesystem_implementation(void)
{
  struct fsinspect_snapshot before_directory;
  struct fsinspect_snapshot directory_state;
  struct fsinspect_snapshot after_write;
  struct fsinspect_snapshot before_boundary;
  struct fsinspect_snapshot after_boundary;
  struct fsinspect_snapshot invalid_before;
  struct fsinspect_snapshot invalid_after;
  struct fsinspect_snapshot invalid_output;
  struct fsinspect_snapshot after_read;
  struct fsinspect_snapshot after_unlink;
  struct fsinspect_snapshot after_close;
  struct fsinspect_snapshot after_cleanup;
  char byte = 'x';
  int unexpected;

  cleanup_fswalk();
  fswalk_snapshot(FSINSPECT_GLOBAL_FD, &before_directory);

  if(mkdir(FSWALK_DIR) < 0)
    fail("cannot create filesystem test directory");
  fswalk_snapshot(FSINSPECT_GLOBAL_FD, &directory_state);
  if(directory_state.allocated_inodes != before_directory.allocated_inodes + 1)
    fail("directory inode allocation delta is not one");
  if(directory_state.allocated_blocks != before_directory.allocated_blocks + 1)
    fail("directory data block allocation delta is not one");

  fswalk_fd = open(FSWALK_PATH, O_CREATE | O_RDWR);
  if(fswalk_fd < 0)
    fail("cannot create filesystem test file");

  for(int logical = 0; logical < FSWALK_BLOCKS; logical++){
    fswalk_fill_block(logical);
    if(write(fswalk_fd, fswalk_block, BSIZE) != BSIZE)
      fail("filesystem block write failed");
  }

  fswalk_snapshot(fswalk_fd, &after_write);
  if(!after_write.has_inode || after_write.inode_type != T_FILE)
    fail("fd snapshot does not describe a regular inode");
  if(after_write.inode_size != (uint64)FSWALK_BLOCKS * BSIZE)
    fail("filesystem file size does not match written blocks");
  if(after_write.allocated_inodes != directory_state.allocated_inodes + 1)
    fail("file inode allocation delta is not one");
  if(after_write.allocated_blocks !=
     directory_state.allocated_blocks + FSWALK_BLOCKS + 1)
    fail("data plus indirect block allocation delta is unexpected");
  if(after_write.direct_first == 0 || after_write.direct_last == 0)
    fail("direct block boundary was not mapped");
  if(after_write.indirect_root == 0 || after_write.indirect_first == 0)
    fail("first indirect block was not mapped");
  if(after_write.direct_first == after_write.direct_last ||
     after_write.direct_last == after_write.indirect_root ||
     after_write.indirect_root == after_write.indirect_first)
    fail("inode block roles unexpectedly alias");
  if(after_write.log.commits <= directory_state.log.commits)
    fail("filesystem writes did not commit redo-log transactions");
  if(after_write.cache.requests <= directory_state.cache.requests)
    fail("filesystem writes did not enter buffer cache");

  fswalk_snapshot(fswalk_fd, &before_boundary);
  if(lseek(fswalk_fd, MAXFILE_BYTES, SEEK_SET) != (int64)MAXFILE_BYTES)
    fail("cannot seek to maximum file boundary");
  if(write(fswalk_fd, &byte, 1) != -1)
    fail("write beyond maximum file size unexpectedly succeeded");
  fswalk_snapshot(fswalk_fd, &after_boundary);
  if(after_boundary.inode_size != before_boundary.inode_size)
    fail("failed boundary write changed file size");
  if(after_boundary.allocated_blocks != before_boundary.allocated_blocks ||
     after_boundary.allocated_inodes != before_boundary.allocated_inodes)
    fail("failed boundary write changed allocation state");

  fswalk_snapshot(FSINSPECT_GLOBAL_FD, &invalid_before);
  if(fsinspect(NOFILE + 1, &invalid_output) != -1)
    fail("invalid descriptor inspection unexpectedly succeeded");
  fswalk_snapshot(FSINSPECT_GLOBAL_FD, &invalid_after);
  if(invalid_after.allocated_blocks != invalid_before.allocated_blocks ||
     invalid_after.allocated_inodes != invalid_before.allocated_inodes)
    fail("invalid inspection changed allocation state");

  if(lseek(fswalk_fd, 0, SEEK_SET) != 0)
    fail("cannot rewind filesystem test file");
  for(int logical = 0; logical < FSWALK_BLOCKS; logical++){
    if(read(fswalk_fd, fswalk_block, BSIZE) != BSIZE)
      fail("filesystem block read failed");
    fswalk_check_block(logical);
  }
  fswalk_snapshot(fswalk_fd, &after_read);
  if(after_read.cache.hits <= after_boundary.cache.hits)
    fail("filesystem read path did not observe buffer cache hits");

  if(unlink(FSWALK_PATH) < 0)
    fail("cannot unlink open filesystem test file");
  unexpected = open(FSWALK_PATH, O_RDONLY);
  if(unexpected >= 0){
    close(unexpected);
    fail("unlinked filesystem path remained reachable");
  }
  fswalk_snapshot(fswalk_fd, &after_unlink);
  if(after_unlink.inode_nlink != 0)
    fail("final unlink did not clear inode link count");
  if(after_unlink.allocated_inodes != after_read.allocated_inodes ||
     after_unlink.allocated_blocks != after_read.allocated_blocks)
    fail("open unlinked inode was reclaimed too early");

  close_checked(fswalk_fd, "cannot close filesystem test file");
  fswalk_fd = -1;
  fswalk_snapshot(FSINSPECT_GLOBAL_FD, &after_close);
  if(after_close.allocated_inodes != directory_state.allocated_inodes)
    fail("final close did not release filesystem inode");
  if(after_close.allocated_blocks != directory_state.allocated_blocks)
    fail("final close did not release data and indirect blocks");

  if(unlink(FSWALK_DIR) < 0)
    fail("cannot remove filesystem test directory");
  fswalk_snapshot(FSINSPECT_GLOBAL_FD, &after_cleanup);
  if(after_cleanup.allocated_inodes != before_directory.allocated_inodes ||
     after_cleanup.allocated_blocks != before_directory.allocated_blocks)
    fail("filesystem cleanup did not restore allocation counts");

  printf("FSWALK layout total=%d data_start=%d inode_start=%d bitmap_start=%d\n",
         after_write.total_blocks, after_write.data_start,
         after_write.inode_start, after_write.bitmap_start);
  printf("FSWALK write inode_delta=1 block_delta=%d direct_first=%d direct_last=%d indirect_root=%d indirect_first=%d\n",
         FSWALK_BLOCKS + 1, after_write.direct_first, after_write.direct_last,
         after_write.indirect_root, after_write.indirect_first);
  printf("FSWALK boundary rejected=1 allocation_unchanged=1\n");
  printf("FSWALK read bytes=%d cache_hit_delta=%d\n",
         FSWALK_BLOCKS * BSIZE,
         (int)(after_read.cache.hits - after_boundary.cache.hits));
  printf("FSWALK unlink nlink=0 retained_until_close=1\n");
  printf("FSWALK reclaim inode_restored=1 block_restored=1 commits=%d\n",
         (int)(after_cleanup.log.commits - before_directory.log.commits));
}

/**
 * 构造文件 API 与文件系统实现的状态闭环，并通过退出状态向 xv6test 报告结果。
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
  test_filesystem_implementation();
  cleanup_workspace();

  printf("fileapitest: verified offsets, links, filesystem mappings, unlink lifetime, and cleanup\n");
  exit(0);
}
