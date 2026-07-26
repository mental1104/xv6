#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "kernel/fcntl.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "user/user.h"

#define fail(msg) do {printf("FAILURE: " msg "\n"); failed = 1; goto done;} while (0);
static int failed = 0;

static char *depth_paths[] = {
  "/testsymlink/d0", "/testsymlink/d1", "/testsymlink/d2",
  "/testsymlink/d3", "/testsymlink/d4", "/testsymlink/d5",
  "/testsymlink/d6", "/testsymlink/d7", "/testsymlink/d8",
  "/testsymlink/d9", "/testsymlink/d10", "/testsymlink/d11",
};

static void testsymlink(void);
static void testdepth(void);
static void concur(void);
static void cleanup(void);

int
main(int argc, char *argv[])
{
  cleanup();
  testsymlink();
  testdepth();
  concur();
  cleanup();
  exit(failed);
}

/** 删除本测试创建的所有路径，使连续运行共享同一镜像时仍从干净状态开始。 */
static void
cleanup(void)
{
  unlink("/testsymlink/a");
  unlink("/testsymlink/b");
  unlink("/testsymlink/c");
  unlink("/testsymlink/1");
  unlink("/testsymlink/2");
  unlink("/testsymlink/3");
  unlink("/testsymlink/4");
  unlink("/testsymlink/z");
  unlink("/testsymlink/y");
  for(int i = 0; i < sizeof(depth_paths) / sizeof(depth_paths[0]); i++)
    unlink(depth_paths[i]);
  unlink("/testsymlink");
}

/**
 * 使用 O_NOFOLLOW 读取符号链接自身的元数据。
 *
 * @param pn 待检查路径。
 * @param st 接收文件状态的调用者缓冲区。
 * @return 成功返回 0；路径无法打开或 fstat 失败时返回 -1。函数始终关闭临时 fd。
 */
static int
stat_slink(char *pn, struct stat *st)
{
  int result = -1;
  int fd = open(pn, O_RDONLY | O_NOFOLLOW);
  if(fd < 0)
    return -1;
  if(fstat(fd, st) == 0)
    result = 0;
  close(fd);
  return result;
}

static void
testsymlink(void)
{
  int r, fd1 = -1, fd2 = -1;
  char buf[4] = {'a', 'b', 'c', 'd'};
  char c = 0, c2 = 0;
  struct stat st;

  printf("Start: test symlinks\n");

  mkdir("/testsymlink");

  fd1 = open("/testsymlink/a", O_CREATE | O_RDWR);
  if(fd1 < 0) fail("failed to open a");

  r = symlink("/testsymlink/a", "/testsymlink/b");
  if(r < 0)
    fail("symlink b -> a failed");

  if(write(fd1, buf, sizeof(buf)) != 4)
    fail("failed to write to a");

  if (stat_slink("/testsymlink/b", &st) != 0)
    fail("failed to stat b");
  if(st.type != T_SYMLINK)
    fail("b isn't a symlink");

  fd2 = open("/testsymlink/b", O_RDWR);
  if(fd2 < 0)
    fail("failed to open b");
  read(fd2, &c, 1);
  if (c != 'a')
    fail("failed to read bytes from b");

  unlink("/testsymlink/a");
  if(open("/testsymlink/b", O_RDWR) >= 0)
    fail("Should not be able to open b after deleting a");

  r = symlink("/testsymlink/b", "/testsymlink/a");
  if(r < 0)
    fail("symlink a -> b failed");

  r = open("/testsymlink/b", O_RDWR);
  if(r >= 0)
    fail("Should not be able to open b (cycle b->a->b->..)\n");

  r = symlink("/testsymlink/nonexistent", "/testsymlink/c");
  if(r != 0)
    fail("Symlinking to nonexistent file should succeed\n");

  r = symlink("/testsymlink/2", "/testsymlink/1");
  if(r) fail("Failed to link 1->2");
  r = symlink("/testsymlink/3", "/testsymlink/2");
  if(r) fail("Failed to link 2->3");
  r = symlink("/testsymlink/4", "/testsymlink/3");
  if(r) fail("Failed to link 3->4");

  close(fd1);
  close(fd2);

  fd1 = open("/testsymlink/4", O_CREATE | O_RDWR);
  if(fd1<0) fail("Failed to create 4\n");
  fd2 = open("/testsymlink/1", O_RDWR);
  if(fd2<0) fail("Failed to open 1\n");

  c = '#';
  r = write(fd2, &c, 1);
  if(r!=1) fail("Failed to write to 1\n");
  r = read(fd1, &c2, 1);
  if(r!=1) fail("Failed to read from 4\n");
  if(c!=c2)
    fail("Value read from 4 differed from value written to 1\n");

  printf("test symlinks: ok\n");
 done:
  close(fd1);
  close(fd2);
}

/** 验证恰好十次符号链接跳转成功，而第十一次跳转被深度上限拒绝。 */
static void
testdepth(void)
{
  int fd = -1;

  printf("Start: test symlink depth boundary\n");

  for(int i = 0; i < 10; i++)
    if(symlink(depth_paths[i + 1], depth_paths[i]) < 0)
      fail("failed to create ten-link chain");

  fd = open(depth_paths[10], O_CREATE | O_RDWR);
  if(fd < 0)
    fail("failed to create ten-link target");
  close(fd);
  fd = -1;

  fd = open(depth_paths[0], O_RDONLY);
  if(fd < 0)
    fail("ten symbolic links should be accepted");
  close(fd);
  fd = -1;

  if(unlink(depth_paths[10]) < 0)
    fail("failed to replace ten-link target");
  if(symlink(depth_paths[11], depth_paths[10]) < 0)
    fail("failed to extend chain to eleven links");

  fd = open(depth_paths[11], O_CREATE | O_RDWR);
  if(fd < 0)
    fail("failed to create eleven-link target");
  close(fd);
  fd = -1;

  fd = open(depth_paths[0], O_RDONLY);
  if(fd >= 0)
    fail("eleven symbolic links should exceed the limit");

  printf("test symlink depth boundary: ok\n");
 done:
  if(fd >= 0)
    close(fd);
}

static void
concur(void)
{
  int pid, i;
  int fd;
  struct stat st;
  int nchild = 2;

  printf("Start: test concurrent symlinks\n");

  fd = open("/testsymlink/z", O_CREATE | O_RDWR);
  if(fd < 0) {
    printf("FAILED: open failed");
    exit(1);
  }
  close(fd);

  for(int j = 0; j < nchild; j++) {
    pid = fork();
    if(pid < 0){
      printf("FAILED: fork failed\n");
      exit(1);
    }
    if(pid == 0) {
      unsigned int x = (pid ? 1 : 97);
      for(i = 0; i < 100; i++){
        x = x * 1103515245 + 12345;
        if((x % 3) == 0) {
          symlink("/testsymlink/z", "/testsymlink/y");
          if (stat_slink("/testsymlink/y", &st) == 0) {
            if(st.type != T_SYMLINK) {
              printf("FAILED: not a symbolic link\n", st.type);
              exit(1);
            }
          }
        } else {
          unlink("/testsymlink/y");
        }
      }
      exit(0);
    }
  }

  int r;
  for(int j = 0; j < nchild; j++) {
    wait(&r);
    if(r != 0) {
      printf("test concurrent symlinks: failed\n");
      exit(1);
    }
  }
  printf("test concurrent symlinks: ok\n");
}
