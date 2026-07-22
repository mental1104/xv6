#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memviz.h"
#include "user/user.h"
#include "user/memvizlib.h"

/**
 * main 是原始 shell 主循环，由 sh.c 提供。
 *
 * @return 原实现会调用 exit()，正常情况下不会返回。
 */
int main(void);

/**
 * sh_entry 为 shell 增加最小启动层，并作为 _sh 的链接入口。
 *
 * @param argc 参数数量。
 * @param argv 参数数组；只有 init 传入的 --login 会触发自动视图。
 * @return 原 shell 主循环的返回值；正常情况下不会返回。
 *
 * 自动采样失败只输出 renderer 的诊断，不阻止 shell 进入命令循环。
 */
int
sh_entry(int argc, char **argv)
{
  if(argc == 2 && strcmp(argv[1], "--login") == 0)
    memviz_print(MEMVIZ_VIEW_USER, 0);

  return main();
}
