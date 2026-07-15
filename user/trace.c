#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/tracemask.h"

/*
 * 输出 trace 命令支持的参数格式。
 *
 * 参数：
 *   program：显示在用法提示开头的可执行程序名称。
 *
 * 返回值：
 *   无。
 */
static void
usage(char *program)
{
  fprintf(2, "Usage: %s mask|syscall[,syscall...] command [args...]\n", program);
}

/*
 * 解析用户指定的跟踪目标，为当前进程设置 trace 掩码，再通过 exec()
 * 将当前包装程序替换为目标命令。
 *
 * 参数：
 *   argc：命令行参数数量，包含 trace 程序自身名称。
 *   argv：参数数组；argv[1] 是 trace 参数，后续元素组成目标命令及其参数。
 *
 * 返回值：
 *   正常路径会由 exec() 替换当前程序而不返回；参数错误、trace 设置失败或
 *   exec() 失败时以状态码 1 退出。
 */
int
main(int argc, char *argv[])
{
  int argument_index; // 复制目标命令参数时使用的 argv 下标。
  int mask;           // 传给现有 trace 系统调用的整数位掩码。
  int parse_status;   // trace_parse_mask() 返回的详细解析状态。
  char *command_argv[MAXARG]; // 传给目标命令、以空指针结尾的参数数组。

  if(argc < 3){
    usage(argv[0]);
    exit(1);
  }

  parse_status = trace_parse_mask(argv[1], &mask);
  if(parse_status != TRACE_MASK_OK){
    if(parse_status == TRACE_MASK_UNKNOWN)
      fprintf(2, "%s: unknown system call in '%s'\n", argv[0], argv[1]);
    else if(parse_status == TRACE_MASK_RANGE)
      fprintf(2, "%s: integer mask out of range: %s\n", argv[0], argv[1]);
    else
      fprintf(2, "%s: invalid trace specification: %s\n", argv[0], argv[1]);
    usage(argv[0]);
    exit(1);
  }

  // 为 exec() 要求的结尾空指针保留一个数组位置。
  if(argc - 2 >= MAXARG){
    fprintf(2, "%s: too many command arguments\n", argv[0]);
    exit(1);
  }

  // trace() 只修改当前进程；随后 exec() 虽然替换程序映像，但会保留该进程
  // 已设置的掩码，因此目标命令及其后续子进程仍沿用原有跟踪流程。
  if(trace(mask) < 0){
    fprintf(2, "%s: trace failed\n", argv[0]);
    exit(1);
  }

  for(argument_index = 2; argument_index < argc; argument_index++)
    command_argv[argument_index - 2] = argv[argument_index];
  command_argv[argc - 2] = 0;

  exec(command_argv[0], command_argv);
  fprintf(2, "%s: exec %s failed\n", argv[0], command_argv[0]);
  exit(1);
}
