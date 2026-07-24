#ifndef XV6_MEMVIZTEST_EXEC_H
#define XV6_MEMVIZTEST_EXEC_H

#include "kernel/types.h"
#include "user/user.h"
#include "user/paths.h"

/**
 * memviztest_exec_absolute 将 memviztest 的唯一外部程序调用固定到 `/usr/bin`。
 *
 * @param path 原测试传入的程序名；当前固定为 `memviz`，函数不依赖其内容。
 * @param argv 传给 memviz 的参数数组，所有权仍归调用者。
 * @return exec() 失败时返回 -1；成功时替换当前进程映像且不会返回。
 *
 * 该头文件只通过 Makefile 强制包含到 memviztest.o。测试主体保持与主线一致，
 * 同时不为 Shell、内核 exec() 或其他用户程序引入 PATH 与兼容搜索行为。
 */
int
memviztest_exec_absolute(char *path, char **argv)
{
  (void)path;
  return exec(XV6_USR_BIN_PATH("memviz"), argv);
}

#define exec memviztest_exec_absolute

#endif
