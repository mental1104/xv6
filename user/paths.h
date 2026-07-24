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

#endif
