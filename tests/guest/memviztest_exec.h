#ifndef XV6_MEMVIZTEST_EXEC_H
#define XV6_MEMVIZTEST_EXEC_H

#include "kernel/types.h"
#include "user/user.h"
#include "user/paths.h"
#include "tests/guest/memviztest_region_mapping.h"
#include "tests/guest/memviztest_swap.h"

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

int memviztest_original_main(int argc, char **argv);

/**
 * 在 memviztest 原入口外增加逻辑区域映射与教学 swap 实验。
 *
 * @param argc 参数数量。
 * @param argv 参数数组；`regions` 或 `swap` 只运行对应概念实验，无参数时在完整回归前运行。
 * @return 原入口的返回值；所有正常路径最终均由测试代码调用 exit()。
 */
int
main(int argc, char **argv)
{
  if(argc == 1){
    memviztest_region_mapping();
    memviztest_swap();
  } else if(argc == 2 && strcmp(argv[1], "regions") == 0){
    memviztest_region_mapping();
    exit(0);
  } else if(argc == 2 && strcmp(argv[1], "swap") == 0){
    memviztest_swap();
    exit(0);
  }

  return memviztest_original_main(argc, argv);
}

#define exec memviztest_exec_absolute
#define main memviztest_original_main

#endif
