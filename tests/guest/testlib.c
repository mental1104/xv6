#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/paths.h"
#include "tests/guest/testlib.h"

/** 描述旧测试命令名与镜像稳定绝对路径的确定映射。 */
struct test_program_path {
  char *name;
  char *path;
};

/**
 * 只为测试捕获助手列出实际会执行的固定用户程序。
 *
 * 这不是 PATH 搜索：未知名称、包含目录的动态路径和故意不存在的程序均原样交给
 * exec()，从而保留错误路径测试语义。
 */
static struct test_program_path test_program_paths[] = {
  {"sleep", XV6_BIN_PATH("sleep")},
  {"find", XV6_BIN_PATH("find")},
  {"xargs", XV6_BIN_PATH("xargs")},
  {"echo", XV6_BIN_PATH("echo")},
  {"pingpong", XV6_USR_BIN_PATH("pingpong")},
  {"primes", XV6_USR_BIN_PATH("primes")},
  {"uthread", XV6_USR_BIN_PATH("uthread")},
  {"varead", XV6_USR_BIN_PATH("varead")},
  {"vawrite", XV6_USR_BIN_PATH("vawrite")},
  {0, 0},
};

/**
 * 返回固定测试程序的绝对路径。
 *
 * @param program argv[0] 中的程序名或调用者提供的路径。
 * @return 命中静态表时返回镜像绝对路径；否则借用并返回 program。
 */
static char *
resolve_test_program(char *program)
{
  for(struct test_program_path *entry = test_program_paths;
      entry->name != 0; entry++)
    if(strcmp(program, entry->name) == 0)
      return entry->path;
  return program;
}

/**
 * 将字符串完整写入文件描述符。
 *
 * @param fd 目标文件描述符。
 * @param data 以 NUL 结尾的待写入字符串。
 * @return 全部写入时返回 0，write() 失败或未前进时返回 -1。
 */
static int
write_all(int fd, char *data)
{
  int length = strlen(data);
  int written = 0;

  while(written < length){
    int count = write(fd, data + written, length - written);
    if(count <= 0)
      return -1;
    written += count;
  }
  return 0;
}

/**
 * 在子进程中执行用户程序，并捕获其标准输出和标准错误。
 *
 * @param argv 传给 exec() 的空指针结尾参数数组；固定程序名会解析为显式绝对路径。
 * @param input 写入子进程标准输入的字符串；为空时继承当前标准输入。
 * @param output 接收捕获文本的缓冲区；始终以 NUL 结尾。
 * @param output_size output 的字节容量，必须大于 0。
 * @param status 接收 wait() 返回的子进程退出状态，不可为空。
 * @return 基础设施成功时返回 0，否则返回 -1；子程序失败通过 status 返回。
 */
int
xv6_test_run_capture(char **argv, char *input, char *output,
                      int output_size, int *status)
{
  int output_pipe[2];
  int input_pipe[2] = {-1, -1};
  int pid;
  int waited_pid;
  int used = 0;
  int infrastructure_failed = 0;
  char chunk[128];
  char *program;

  if(argv == 0 || argv[0] == 0 || output == 0 || output_size <= 0 || status == 0)
    return -1;
  output[0] = 0;
  program = resolve_test_program(argv[0]);

  if(pipe(output_pipe) < 0)
    return -1;
  if(input != 0 && pipe(input_pipe) < 0){
    close(output_pipe[0]);
    close(output_pipe[1]);
    return -1;
  }

  pid = fork();
  if(pid < 0){
    close(output_pipe[0]);
    close(output_pipe[1]);
    if(input != 0){
      close(input_pipe[0]);
      close(input_pipe[1]);
    }
    return -1;
  }

  if(pid == 0){
    close(output_pipe[0]);

    // 关闭后 dup() 会取得最小可用描述符，使捕获管道稳定占据 stdout/stderr。
    close(1);
    if(dup(output_pipe[1]) != 1)
      exit(126);
    close(2);
    if(dup(output_pipe[1]) != 2)
      exit(126);
    close(output_pipe[1]);

    if(input != 0){
      close(input_pipe[1]);
      close(0);
      if(dup(input_pipe[0]) != 0)
        exit(126);
      close(input_pipe[0]);
    }

    exec(program, argv);
    exit(127);
  }

  close(output_pipe[1]);
  if(input != 0){
    close(input_pipe[0]);
    if(write_all(input_pipe[1], input) < 0)
      infrastructure_failed = 1;
    close(input_pipe[1]);
  }

  // 即使 output 已满也继续读取并丢弃尾部，避免子进程因管道写满而永久阻塞。
  for(;;){
    int count = read(output_pipe[0], chunk, sizeof(chunk));
    if(count < 0){
      infrastructure_failed = 1;
      break;
    }
    if(count == 0)
      break;
    for(int i = 0; i < count && used + 1 < output_size; i++)
      output[used++] = chunk[i];
  }
  output[used] = 0;
  close(output_pipe[0]);

  waited_pid = wait(status);
  if(waited_pid != pid)
    infrastructure_failed = 1;

  return infrastructure_failed ? -1 : 0;
}

/**
 * 判断完整文本中是否包含指定连续子串。
 *
 * @param text 以 NUL 结尾的待搜索文本。
 * @param needle 以 NUL 结尾的非空目标子串。
 * @return 找到时返回 1，否则返回 0。
 */
int
xv6_test_contains(char *text, char *needle)
{
  if(text == 0 || needle == 0 || needle[0] == 0)
    return 0;

  for(int index = 0; text[index] != 0; index++){
    int matched = 1;

    for(int offset = 0; needle[offset] != 0; offset++){
      if(text[index + offset] == 0 || text[index + offset] != needle[offset]){
        matched = 0;
        break;
      }
    }
    if(matched)
      return 1;
  }
  return 0;
}
