#ifndef XV6_USER_VAACCESSLIB_H
#define XV6_USER_VAACCESSLIB_H

#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memviz.h"

// 用户态 VA 探针支持的访问操作。
enum vaaccess_op {
  VAACCESS_READ = 1,
  VAACCESS_WRITE = 2,
};

// 用户态 VA 探针支持的实验模式。
enum vaaccess_mode {
  VAACCESS_MODE_DIRECT = 1,
  VAACCESS_MODE_LAZY = 2,
  VAACCESS_MODE_COW = 3,
};

/**
 * varead 命令入口的共享实现。
 *
 * @param argc 命令行参数数量。
 * @param argv 参数数组；支持普通地址、--lazy、--expect 和 --snapshot。
 * @return 进程退出状态；0 表示实际结果符合预期。
 */
int vaaccess_read_main(int argc, char **argv);

/**
 * vawrite 命令入口的共享实现。
 *
 * @param argc 命令行参数数量。
 * @param argv 参数数组；支持普通地址、--lazy、--cow、--expect 和 --snapshot。
 * @return 进程退出状态；0 表示实际结果符合预期。
 */
int vaaccess_write_main(int argc, char **argv);

/**
 * vaprobe 交互入口的共享实现。
 *
 * @param argc 命令行参数数量；首版不接受额外参数。
 * @param argv 参数数组，仅用于保持 main 签名一致。
 * @return 交互会话正常 quit 时返回 0，参数错误返回 2。
 */
int vaaccess_probe_main(int argc, char **argv);

/**
 * vaaccess_parse_u64 解析非负十进制或 0x 十六进制整数。
 *
 * @param text 输入字符串，必须完整表示一个非负整数。
 * @param value 接收解析结果，不可为空。
 * @return 成功返回 0；空串、负数、非法字符或 uint64 溢出返回 -1。
 */
int vaaccess_parse_u64(char *text, uint64 *value);

#endif
