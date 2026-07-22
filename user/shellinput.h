#ifndef XV6_USER_SHELLINPUT_H
#define XV6_USER_SHELLINPUT_H

struct shell_history;

/**
 * 从 stdin 读取一条 Shell 命令，并在交互式 console 上提供最小行编辑。
 *
 * @param buf 输出命令缓冲区。
 * @param max 缓冲区容量，必须大于 1。
 * @param history 当前 Shell 会话历史，只读，用于方向键召回。
 * @return 成功提交一行返回 0；空行 Ctrl-D、读取失败或 EOF 返回 -1。
 */
int shell_readline(char *buf, int max, const struct shell_history *history);

#endif
