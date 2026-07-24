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

/** 将 execve() 传入的环境向量交给 sh.c 中的 Shell 执行层。 */
void shell_set_environment(char **envp);

/**
 * 仅为自动化回归输出当前进程收到的环境向量并退出。
 *
 * @param envp 以空指针结尾的环境数组。
 */
static void
print_environment_and_exit(char **envp)
{
  if(envp != 0)
    for(char **entry = envp; *entry != 0; entry++)
      printf("%s\n", *entry);
  exit(0);
}

/**
 * sh_entry 为 shell 增加最小启动层，并作为 _sh 的链接入口。
 *
 * @param argc 参数数量。
 * @param argv 参数数组；init 的 --login 会触发自动内存视图。
 * @param envp 由 execve() 建立的环境数组；Shell 后续命令继续继承它。
 * @return 原 shell 主循环的返回值；正常情况下不会返回。
 *
 * `--print-environment` 是不进入交互循环的测试入口，用于直接验证 a2/envp ABI。
 * 自动采样失败只输出 renderer 的诊断，不阻止 shell 进入命令循环。
 */
int
sh_entry(int argc, char **argv, char **envp)
{
  shell_set_environment(envp);
  if(argc == 2 && strcmp(argv[1], "--print-environment") == 0)
    print_environment_and_exit(envp);
  if(argc == 2 && strcmp(argv[1], "--login") == 0)
    memviz_print(MEMVIZ_VIEW_USER, 0);

  return main();
}
