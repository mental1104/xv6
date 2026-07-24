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
 * xv6_usertests_path_strcmp 为 usertests 的固定路径识别提供地址边界保护。
 *
 * @param left 待判断的路径；可能是故意传给内核的非法用户地址。
 * @param right 适配层持有的有效 NUL 结尾字符串。
 * @return 与 strcmp 一致的零或非零结果；越过当前进程大小时视为不相等。
 *
 * pgbug 和 copyinstr 系列会把越界或贴近地址空间末端的路径交给系统调用。
 * 适配器只能读取当前进程大小以内的字节，不能在进入内核前破坏错误路径测试。
 */
static inline int
xv6_usertests_path_strcmp(const char *left, const char *right)
{
  uint64 address = (uint64)left;
  uint64 process_size = (uint64)sbrk(0);

  if(address == 0 || address >= process_size)
    return 1;
  for(uint64 offset = 0; ; offset++){
    if(address + offset < address || address + offset >= process_size)
      return 1;
    unsigned char left_char = left[offset];
    unsigned char right_char = right[offset];
    if(left_char != right_char)
      return left_char - right_char;
    if(left_char == 0)
      return 0;
  }
}

#define strcmp xv6_usertests_path_strcmp

/**
 * xv6_usertests_open_absolute 将上游测试引用的固定镜像文件映射到绝对路径。
 *
 * @param path 传给 open() 的路径；动态、临时和非法路径保持原样。
 * @param mode open() 标志位，原样传递给系统调用。
 * @return open() 返回的文件描述符或 -1。
 *
 * 只识别 README、init 和 echo 三个构建期镜像文件。copyinstr 系列使用的非法
 * 路径不会命中字面量，仍由内核 copyinstr() 验证，避免引入通用搜索行为。
 */
static inline int
xv6_usertests_open_absolute(const char *path, int mode)
{
  if(strcmp(path, "README") == 0)
    return open("/README", mode);
  if(strcmp(path, "init") == 0)
    return open("/init", mode);
  if(strcmp(path, "echo") == 0)
    return open(XV6_BIN_PATH("echo"), mode);
  return open(path, mode);
}

#define open xv6_usertests_open_absolute

/**
 * xv6_usertests_mkdir_relative 消除 subdir 对启动目录为 `/` 的单点假设。
 *
 * @param path 传给 mkdir() 的路径。
 * @return mkdir() 的原始返回值。
 *
 * subdir 先用相对路径创建 `dd`，随后却写死 `/dd/dd`。Shell 改从 `/root`
 * 启动后，两者不再属于同一棵测试目录。只将该字面量改回 `dd/dd`，其余目录
 * 创建保持原样，使临时测试数据继续留在当前测试目录而不是污染根目录。
 */
static inline int
xv6_usertests_mkdir_relative(const char *path)
{
  if(strcmp(path, "/dd/dd") == 0)
    return mkdir("dd/dd");
  return mkdir(path);
}

#define mkdir xv6_usertests_mkdir_relative
#endif

#endif
