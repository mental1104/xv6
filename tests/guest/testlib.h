#ifndef XV6_TESTLIB_H
#define XV6_TESTLIB_H

/**
 * 在子进程中执行用户程序，并捕获其标准输出和标准错误。
 *
 * @param argv 传给 exec() 的空指针结尾参数数组；argv[0] 是程序名。
 * @param input 写入子进程标准输入的字符串；为空时继承当前标准输入。
 * @param output 接收捕获文本的缓冲区；始终以 NUL 结尾。
 * @param output_size output 的字节容量，必须大于 0。
 * @param status 接收 wait() 返回的子进程退出状态，不可为空。
 * @return fork、pipe、write、read、wait 等基础设施成功时返回 0，否则返回 -1。
 *         子程序自身失败通过 status 返回，不会被转换为基础设施错误。
 */
int xv6_test_run_capture(char **argv, char *input, char *output,
                          int output_size, int *status);

/**
 * 判断完整文本中是否包含指定连续子串。
 *
 * @param text 以 NUL 结尾的待搜索文本。
 * @param needle 以 NUL 结尾的非空目标子串。
 * @return 找到时返回 1，否则返回 0。
 */
int xv6_test_contains(char *text, char *needle);

#endif
