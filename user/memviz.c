#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memviz.h"
#include "user/user.h"
#include "user/memvizlib.h"

/**
 * usage 输出 memviz 支持的视图和纯文本选项。
 */
static void
usage(void)
{
  fprintf(2, "usage: memviz <user|phys|kernel|all> [--plain]\n");
}

/**
 * main 解析命令行并打印指定的当前进程内存视图。
 *
 * @param argc 参数数量。
 * @param argv 参数数组；第一个位置参数必须是视图名。
 * @return 成功返回 0；参数或采样失败返回 1。
 */
int
main(int argc, char **argv)
{
  if(argc < 2 || argc > 3){
    usage();
    exit(1);
  }

  int plain = 0;
  if(argc == 3){
    if(strcmp(argv[2], "--plain") != 0){
      usage();
      exit(1);
    }
    plain = 1;
  }

  int result;
  if(strcmp(argv[1], "user") == 0)
    result = memviz_print(MEMVIZ_VIEW_USER, plain);
  else if(strcmp(argv[1], "phys") == 0)
    result = memviz_print(MEMVIZ_VIEW_PHYS, plain);
  else if(strcmp(argv[1], "kernel") == 0)
    result = memviz_print(MEMVIZ_VIEW_KERNEL, plain);
  else if(strcmp(argv[1], "all") == 0)
    result = memviz_print_all(plain);
  else {
    usage();
    exit(1);
  }

  exit(result == 0 ? 0 : 1);
}
