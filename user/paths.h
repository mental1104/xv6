#ifndef XV6_USER_PATHS_H
#define XV6_USER_PATHS_H

/**
 * 定义启动镜像内稳定的绝对路径。
 *
 * 本教学布局不实现 PATH、环境变量或隐式命令搜索。调用 exec() 的代码必须显式
 * 选择 `/bin`、`/usr/bin` 或内部测试目录，避免把 Shell 行为误认为内核行为。
 */
#define XV6_ROOT_HOME "/root"
#define XV6_CONSOLE_PATH "/console"
#define XV6_BIN_PATH(name) "/bin/" name
#define XV6_USR_BIN_PATH(name) "/usr/bin/" name
#define XV6_TEST_PATH(name) "/usr/lib/xv6/tests/" name

#ifdef XV6_USERTESTS_EXEC_ADAPTER
/**
 * xv6_usertests_path_strcmp 为 usertests 的 exec 路径适配提供边界保护。
 *
 * @param left 待判断的 exec 路径；可能是故意传给内核的非法用户地址。
 * @param right 适配层持有的固定程序名，必须指向有效的 NUL 结尾字符串。
 * @return left 位于当前进程用户地址范围外时返回非零；否则返回普通 strcmp 结果。
 *
 * 上游 pgbug 会故意把越界地址传给 exec()，验证内核 copyinstr 错误路径。路径
 * 适配器不能在系统调用前解引用该地址，否则测试会在用户态提前死亡。该保护只在
 * usertests_2g.o 的目标级编译开关下启用，不改变其他用户程序的 strcmp 语义。
 */
static inline int
xv6_usertests_path_strcmp(const char *left, const char *right)
{
  uint64 address = (uint64)left;
  uint64 process_size = (uint64)sbrk(0);

  if(address == 0 || address >= process_size)
    return 1;
  return strcmp(left, right);
}

#define strcmp xv6_usertests_path_strcmp
#endif

#endif
