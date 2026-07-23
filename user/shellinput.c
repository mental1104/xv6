#include "kernel/types.h"
#include "kernel/console.h"
#include "user/history.h"
#include "user/shellinput.h"
#include "user/user.h"

#define ESCAPE 0x1b
#define CTRL_D 0x04
#define CTRL_U 0x15

enum escape_state {
  ESCAPE_NONE,
  ESCAPE_STARTED,
  ESCAPE_CSI,
};

/**
 * 将一段文本完整写入指定文件描述符。
 *
 * @param fd 目标文件描述符；Shell 编辑回显使用 stderr 对应的 console。
 * @param text NUL 结尾文本。
 */
static void
write_text(int fd, const char *text)
{
  write(fd, text, strlen(text));
}

/**
 * 擦除当前命令行中由 Shell 用户态回显的全部字符。
 *
 * @param length 当前可见字符数。
 */
static void
erase_line(int length)
{
  while(length-- > 0)
    write_text(2, "\b \b");
}

/**
 * 用历史命令或草稿替换当前行，并保持缓冲区与终端显示一致。
 *
 * @param buf Shell 命令缓冲区。
 * @param max 缓冲区容量。
 * @param length 输入输出参数，调用前为旧长度，返回时为新长度。
 * @param replacement 待显示的新文本。
 */
static void
replace_line(char *buf, int max, int *length, const char *replacement)
{
  int new_length = 0;

  erase_line(*length);
  while(replacement[new_length] != 0 && new_length + 1 < max){
    buf[new_length] = replacement[new_length];
    new_length++;
  }
  buf[new_length] = 0;
  write(2, buf, new_length);
  *length = new_length;
}

/**
 * 在方向键状态机中处理一个字节。
 *
 * @param state 当前 Escape 解析状态，会被更新。
 * @param c 本次输入字节。
 * @param history 当前 Shell 会话历史，只读。
 * @param buf 当前命令缓冲区。
 * @param max 缓冲区容量。
 * @param length 当前命令长度，会在召回时更新。
 * @param cursor 当前历史浏览游标。
 * @return 字节已被 Escape 状态机消费时返回 1；应继续普通字符处理时返回 0。
 */
static int
handle_escape(enum escape_state *state, char c,
              const struct shell_history *history,
              char *buf, int max, int *length,
              struct shell_history_cursor *cursor)
{
  char replacement[SHELL_HISTORY_COMMAND_SIZE];

  if(*state == ESCAPE_NONE){
    if((unsigned char)c == ESCAPE){
      *state = ESCAPE_STARTED;
      return 1;
    }
    return 0;
  }

  if(*state == ESCAPE_STARTED){
    if(c == '['){
      *state = ESCAPE_CSI;
      return 1;
    }
    // 非 CSI 序列只丢弃 ESC；当前字节重新按普通输入处理，避免吞掉可见字符。
    *state = ESCAPE_NONE;
    return 0;
  }

  *state = ESCAPE_NONE;
  if(c == 'A'){
    if(shell_history_cursor_up(history, cursor, buf,
                               replacement, sizeof(replacement)))
      replace_line(buf, max, length, replacement);
  } else if(c == 'B'){
    if(shell_history_cursor_down(history, cursor,
                                 replacement, sizeof(replacement)))
      replace_line(buf, max, length, replacement);
  }
  // 其他 CSI 尾字节被安全忽略；状态在本次输入后立即复位，不额外阻塞等待。
  return 1;
}

/**
 * 将已编辑命令转换为原 sh.c 期待的带换行输入。
 *
 * @param buf 命令缓冲区。
 * @param max 缓冲区容量。
 * @param length 当前有效字符数。
 */
static void
terminate_line(char *buf, int max, int length)
{
  if(length + 1 < max)
    buf[length++] = '\n';
  buf[length] = 0;
}

/**
 * 从 stdin 读取一条 Shell 命令，并在交互式 console 上提供最小行编辑。
 *
 * @param buf 输出命令缓冲区。
 * @param max 缓冲区容量，必须大于 1。
 * @param history 当前 Shell 会话历史，只读，用于方向键召回。
 * @return 成功提交一行返回 0；空行 Ctrl-D、读取失败或 EOF 返回 -1。
 *
 * stdin 不是 console 或存在尚未消费的 cooked 输入时，raw claim 会失败并回退到
 * 原 gets()。raw mode 只覆盖当前提示符读取，正常结束路径恢复 cooked mode；Shell
 * 异常退出时由 fileclose() 按 owner PID 与 console file 对象回收 raw mode。
 */
int
shell_readline(char *buf, int max, const struct shell_history *history)
{
  struct shell_history_cursor cursor;
  enum escape_state escape = ESCAPE_NONE;
  int length = 0;

  shell_history_cursor_reset(&cursor);
  if(max < 2){
    if(max > 0)
      buf[0] = 0;
    return -1;
  }

  consolemode(0, CONSOLE_MODE_COOKED);
  if(consolemode(0, CONSOLE_MODE_RAW) < 0){
    gets(buf, max);
    return buf[0] == 0 ? -1 : 0;
  }

  memset(buf, 0, max);
  for(;;){
    char c;
    int count = read(0, &c, 1);

    if(count != 1){
      consolemode(0, CONSOLE_MODE_COOKED);
      buf[0] = 0;
      return -1;
    }
    if(handle_escape(&escape, c, history, buf, max, &length, &cursor))
      continue;

    if(c == '\r' || c == '\n'){
      write_text(2, "\n");
      consolemode(0, CONSOLE_MODE_COOKED);
      terminate_line(buf, max, length);
      return 0;
    }
    if((unsigned char)c == CTRL_D){
      consolemode(0, CONSOLE_MODE_COOKED);
      if(length == 0){
        buf[0] = 0;
        return -1;
      }
      // 非空行 Ctrl-D 按“立即提交当前输入”处理，避免静默丢弃已经编辑的命令。
      write_text(2, "\n");
      terminate_line(buf, max, length);
      return 0;
    }
    if((unsigned char)c == CTRL_U){
      erase_line(length);
      length = 0;
      buf[0] = 0;
      shell_history_cursor_edit(&cursor);
      escape = ESCAPE_NONE;
      continue;
    }
    if(c == '\b' || (unsigned char)c == 0x7f){
      if(length > 0){
        length--;
        buf[length] = 0;
        write_text(2, "\b \b");
        shell_history_cursor_edit(&cursor);
      }
      escape = ESCAPE_NONE;
      continue;
    }
    if(c >= 0x20 && c <= 0x7e){
      if(length + 1 < max){
        buf[length++] = c;
        buf[length] = 0;
        write(2, &c, 1);
        shell_history_cursor_edit(&cursor);
      }
      continue;
    }
    // 其余控制字节不修改缓冲区，也不会改变历史游标边界。
  }
}
