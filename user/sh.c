// Shell composition unit.

#include "user/paths.h"

// 保持原 Shell 实现及其静态状态位于同一个翻译单元，使启动层可以在进入主循环前
// 同步 PID 1 已建立的 `/root` 工作目录，而无需新增 getcwd 系统调用。
#define main shell_main
#include "user/shcore.inc"
#undef main

/**
 * 将 Shell 的逻辑目录同步为 PID 1 已切换到的 root 用户主目录。
 *
 * @return 原 Shell 主循环的返回值；正常情况下主循环通过 exit() 结束进程。
 *
 * shell_cwd 只负责提示符和 `cd` 的逻辑路径规范化，真实 cwd 已由 init 在 exec
 * 前切换。两者必须使用同一个绝对起点，否则相对 `cd` 会显示错误路径。
 */
int
main(void)
{
  int length = strlen(XV6_ROOT_HOME);

  if(length + 1 > sizeof(shell_cwd)){
    fprintf(2, "sh: initial cwd is too long\n");
    exit(1);
  }
  memmove(shell_cwd, XV6_ROOT_HOME, length + 1);
  shell_cwd_known = 1;
  return shell_main();
}
