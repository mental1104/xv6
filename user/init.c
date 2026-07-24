// init: The initial user-level program

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "user/user.h"
#include "user/paths.h"
#include "kernel/fcntl.h"

// --login 只由 init 传入，使首个交互 shell 在 prompt 前打印一次自身内存视图。
char *argv[] = { XV6_BIN_PATH("sh"), "--login", 0 };

/** 描述一个从历史根目录位置迁移到稳定绝对路径的初始镜像文件。 */
struct image_file {
  char *source;
  char *destination;
};

/**
 * 目录按父目录优先顺序创建；空目录仍用于表达教学职责边界。
 *
 * `/sys`、`/mnt` 和 `/lib` 当前只是占位，不代表已经实现 sysfs、mount 或动态链接。
 */
static char *image_directories[] = {
  "/etc", "/bin", "/usr", "/home", "/mnt", "/root", "/sys",
  "/var", "/tmp", "/lib", "/usr/bin", "/usr/lib", "/usr/lib/xv6",
  "/usr/lib/xv6/tests", 0,
};

/**
 * 把 mkfs 仍平铺写入根目录的预编译程序迁移到教学版运行时布局。
 *
 * `/init`、`/console` 和 `/README` 保留在根目录。普通命令、教学工具和测试实现
 * 分别进入 `/bin`、`/usr/bin` 和 `/usr/lib/xv6/tests`。
 */
static struct image_file image_files[] = {
  {"/cat", XV6_BIN_PATH("cat")},
  {"/echo", XV6_BIN_PATH("echo")},
  {"/grep", XV6_BIN_PATH("grep")},
  {"/kill", XV6_BIN_PATH("kill")},
  {"/ln", XV6_BIN_PATH("ln")},
  {"/ls", XV6_BIN_PATH("ls")},
  {"/mkdir", XV6_BIN_PATH("mkdir")},
  {"/rm", XV6_BIN_PATH("rm")},
  {"/sh", XV6_BIN_PATH("sh")},
  {"/wc", XV6_BIN_PATH("wc")},
  {"/sleep", XV6_BIN_PATH("sleep")},
  {"/find", XV6_BIN_PATH("find")},
  {"/xargs", XV6_BIN_PATH("xargs")},

  {"/memviz", XV6_USR_BIN_PATH("memviz")},
  {"/schedviz", XV6_USR_BIN_PATH("schedviz")},
  {"/varead", XV6_USR_BIN_PATH("varead")},
  {"/vawrite", XV6_USR_BIN_PATH("vawrite")},
  {"/vaprobe", XV6_USR_BIN_PATH("vaprobe")},
  {"/zombie", XV6_USR_BIN_PATH("zombie")},
  {"/pingpong", XV6_USR_BIN_PATH("pingpong")},
  {"/primes", XV6_USR_BIN_PATH("primes")},
  {"/trace", XV6_USR_BIN_PATH("trace")},
  {"/call", XV6_USR_BIN_PATH("call")},
  {"/uthread", XV6_USR_BIN_PATH("uthread")},
  {"/xv6test", XV6_USR_BIN_PATH("xv6test")},

  {"/memviztest", XV6_TEST_PATH("memviztest")},
  {"/vaaccesstest", XV6_TEST_PATH("vaaccesstest")},
  {"/addresswindowtest", XV6_TEST_PATH("addresswindowtest")},
  {"/forktest", XV6_TEST_PATH("forktest")},
  {"/stressfs", XV6_TEST_PATH("stressfs")},
  {"/usertests", XV6_TEST_PATH("usertests")},
  {"/grind", XV6_TEST_PATH("grind")},
  {"/tracemasktest", XV6_TEST_PATH("tracemasktest")},
  {"/sysinfotest", XV6_TEST_PATH("sysinfotest")},
  {"/bttest", XV6_TEST_PATH("bttest")},
  {"/alarmtest", XV6_TEST_PATH("alarmtest")},
  {"/lazytests", XV6_TEST_PATH("lazytests")},
  {"/cowtest", XV6_TEST_PATH("cowtest")},
  {"/bigfile", XV6_TEST_PATH("bigfile")},
  {"/largefile", XV6_TEST_PATH("largefile")},
  {"/symlinktest", XV6_TEST_PATH("symlinktest")},
  {"/mmaptest", XV6_TEST_PATH("mmaptest")},
  {"/lab1test", XV6_TEST_PATH("lab1test")},
  {"/tracesmoke", XV6_TEST_PATH("tracesmoke")},
  {"/uthreadtest", XV6_TEST_PATH("uthreadtest")},
  {"/historytest", XV6_TEST_PATH("historytest")},
  {"/consolelinetest", XV6_TEST_PATH("consolelinetest")},
  {"/lstest", XV6_TEST_PATH("lstest")},
  {"/schedtest", XV6_TEST_PATH("schedtest")},
  {"/schedtracetest", XV6_TEST_PATH("schedtracetest")},
  {"/xargstest.sh", XV6_TEST_PATH("xargstest.sh")},
  {0, 0},
};

/**
 * 打印不可恢复的启动布局错误并停止 PID 1 继续破坏文件系统。
 *
 * @param operation 失败的布局操作。
 * @param path 相关绝对路径。
 */
static void
layout_fail(char *operation, char *path)
{
  printf("init: layout %s failed: %s\n", operation, path);
  for(;;)
    sleep(1000);
}

/**
 * 确保一个目录存在且类型正确。
 *
 * @param path 必须按父目录优先顺序提供的绝对目录路径。
 */
static void
ensure_directory(char *path)
{
  struct stat st;

  if(stat(path, &st) == 0){
    if(st.type != T_DIR)
      layout_fail("not a directory", path);
    return;
  }
  if(mkdir(path) < 0)
    layout_fail("mkdir", path);
}

/**
 * 通过硬链接加删除实现同文件系统内的幂等文件迁移。
 *
 * @param source mkfs 初始平铺镜像中的根目录路径。
 * @param destination 最终稳定绝对路径。
 *
 * 目标已存在时验证源目标 inode 一致后删除旧入口；源已不存在则说明此前启动已经
 * 完成迁移。这样 Shell 退出后 init 重启，或首次启动中途被打断，都不会重复复制。
 */
static void
place_file(char *source, char *destination)
{
  struct stat source_stat;
  struct stat destination_stat;
  int source_exists = stat(source, &source_stat) == 0;
  int destination_exists = stat(destination, &destination_stat) == 0;

  if(destination_exists){
    if(source_exists){
      if(source_stat.ino != destination_stat.ino)
        layout_fail("destination conflict", destination);
      if(unlink(source) < 0)
        layout_fail("unlink source", source);
    }
    return;
  }

  if(!source_exists)
    layout_fail("missing source", source);
  if(link(source, destination) < 0)
    layout_fail("link", destination);
  if(unlink(source) < 0)
    layout_fail("unlink source", source);
}

/**
 * 确保固定数据文件在用户主目录中有一个兼容硬链接，但保留根目录原入口。
 *
 * @param source 需要保留的权威绝对路径。
 * @param destination 旧测试从 `/root` 使用相对路径访问的兼容入口。
 */
static void
ensure_file_link(char *source, char *destination)
{
  struct stat source_stat;
  struct stat destination_stat;

  if(stat(source, &source_stat) < 0)
    layout_fail("missing link source", source);
  if(stat(destination, &destination_stat) == 0){
    if(source_stat.ino != destination_stat.ino)
      layout_fail("link conflict", destination);
    return;
  }
  if(link(source, destination) < 0)
    layout_fail("link data", destination);
}

/** 在首个 Shell 启动前建立并验证教学版目录布局。 */
static void
setup_image_layout(void)
{
  for(char **directory = image_directories; *directory != 0; directory++)
    ensure_directory(*directory);
  for(struct image_file *file = image_files; file->source != 0; file++)
    place_file(file->source, file->destination);

  // 原始 usertests 的 copyout 用例读取相对路径 README；它不是可执行文件搜索。
  ensure_file_link("/README", "/root/README");
}

int
main(void)
{
  int pid, wpid;

  if(open(XV6_CONSOLE_PATH, O_RDWR) < 0){
    mknod(XV6_CONSOLE_PATH, CONSOLE, 0);
    open(XV6_CONSOLE_PATH, O_RDWR);
  }
  dup(0);  // stdout
  dup(0);  // stderr

  setup_image_layout();
  if(chdir(XV6_ROOT_HOME) < 0)
    layout_fail("chdir", XV6_ROOT_HOME);

  for(;;){
    printf("init: starting sh\n");
    pid = fork();
    if(pid < 0){
      printf("init: fork failed\n");
      exit(1);
    }
    if(pid == 0){
      exec(XV6_BIN_PATH("sh"), argv);
      printf("init: exec sh failed\n");
      exit(1);
    }

    for(;;){
      // this call to wait() returns if the shell exits,
      // or if a parentless process exits.
      wpid = wait((int *) 0);
      if(wpid == pid){
        // the shell exited; restart it.
        break;
      } else if(wpid < 0){
        printf("init: wait returned an error\n");
        exit(1);
      } else {
        // it was a parentless process; do nothing.
      }
    }
  }
}
