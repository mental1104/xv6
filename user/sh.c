// Shell composition unit.

#include "kernel/types.h"
#include "user/paths.h"
#include "user/user.h"

static char *empty_environment[] = {0};
static char **shell_environment = empty_environment;

/**
 * 保存 execve() 传给 Shell 的环境向量。
 *
 * @param envp 由当前 Shell 用户栈持有的环境数组；0 等价于空环境。
 *
 * Shell 不复制字符串，也不把环境移入内核对象。当前进程栈在整个交互会话中持续存在，
 * fork() 会自然复制这些页面，后续 execve() 再把同一向量交给目标程序。
 */
void
shell_set_environment(char **envp)
{
  shell_environment = envp == 0 ? empty_environment : envp;
}

/** 在命令子进程中输出当前只读环境；该进程随后立即退出，不修改父 Shell 状态。 */
static void
shell_print_environment(void)
{
  char **entry;

  for(entry = shell_environment; *entry != 0; entry++)
    printf("%s\n", *entry);
}

/**
 * 代替原 Shell 对 exec() 的直接调用，并保留失败返回语义。
 *
 * @param program 用户输入的命令名或显式路径。
 * @param argv 解析后的参数数组。
 * @return PATH 中全部候选执行失败时返回 -1；成功后不返回。
 *
 * `env` 是当前教学 Shell 的只读内置工具，运行在命令子进程中。其他命令统一通过
 * execvpe() 搜索，内核 execve() 本身从不解释 PATH。
 */
static int
shell_exec(char *program, char **argv)
{
  if(strcmp(program, "env") == 0){
    if(argv[1] != 0){
      fprintf(2, "Usage: env\n");
      exit(2);
    }
    shell_print_environment();
    exit(0);
  }
  return execvpe(program, argv, shell_environment);
}

// 保持原 Shell 实现及其静态状态位于同一个翻译单元，使启动层可以在进入主循环前
// 同步 PID 1 已建立的 `/root` 工作目录，而无需新增 getcwd 系统调用。
#define exec shell_exec
#define main shell_main
#include "user/shcore.inc"
#undef main
#undef exec

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
