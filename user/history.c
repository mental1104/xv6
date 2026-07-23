#include "kernel/types.h"
#include "user/history.h"
#include "user/user.h"

/**
 * 判断字符是否属于 Shell 历史过滤使用的空白集合。
 *
 * @param c 待检查的单字节字符。
 * @return 空格、制表、垂直制表或行尾字符返回 1，否则返回 0。
 */
static int
history_space(char c)
{
  return c == ' ' || c == '\t' || c == '\v' || c == '\r' || c == '\n';
}

/**
 * 将命令复制到固定容量缓冲区，并去除末尾 CR/LF。
 *
 * @param dst 输出缓冲区，必须至少提供 dst_size 个字节。
 * @param dst_size 输出容量，必须大于 0。
 * @param src 输入命令；超长部分会被截断以保证 NUL 结尾。
 * @return 复制后命令长度，不包含结尾 NUL。
 */
static int
history_copy_command(char *dst, int dst_size, const char *src)
{
  int length = 0;

  while(src[length] != 0 && length + 1 < dst_size){
    dst[length] = src[length];
    length++;
  }
  while(length > 0 && (dst[length - 1] == '\r' || dst[length - 1] == '\n'))
    length--;
  dst[length] = 0;
  return length;
}

/**
 * 判断命令是否只包含历史规则认可的空白字符。
 *
 * @param command 已保证 NUL 结尾的命令文本。
 * @return 空命令或纯空白命令返回 1，否则返回 0。
 */
static int
history_blank(const char *command)
{
  for(; *command != 0; command++)
    if(!history_space(*command))
      return 0;
  return 1;
}

/**
 * 初始化一份空的会话历史。
 *
 * @param history 待初始化的历史对象，调用者持有其存储。
 */
void
shell_history_init(struct shell_history *history)
{
  memset(history, 0, sizeof(*history));
  history->next_number = 1;
}

/**
 * 按从旧到新的逻辑下标读取历史记录。
 *
 * @param history 只读历史对象。
 * @param index 逻辑下标，0 表示当前最旧记录。
 * @return 下标有效时返回内部只读记录指针；越界时返回 0，所有权仍归 history。
 */
const struct shell_history_entry *
shell_history_at(const struct shell_history *history, int index)
{
  if(index < 0 || index >= history->count)
    return 0;
  return &history->entries[(history->start + index) % SHELL_HISTORY_CAPACITY];
}

/**
 * 返回当前有效历史记录数量。
 *
 * @param history 只读历史对象。
 * @return 0 到 SHELL_HISTORY_CAPACITY 之间的记录数。
 */
int
shell_history_count(const struct shell_history *history)
{
  return history->count;
}

/**
 * 将一条有效命令追加到环形历史表。
 *
 * @param history 待修改的会话历史。
 * @param command 原始命令；函数会去除末尾 CR/LF，并过滤空白与连续重复。
 * @return 实际新增记录返回 1；被过滤或与上一条完全相同返回 0。
 */
int
shell_history_add(struct shell_history *history, const char *command)
{
  char normalized[SHELL_HISTORY_COMMAND_SIZE];
  const struct shell_history_entry *latest;
  struct shell_history_entry *entry;
  int index;

  history_copy_command(normalized, sizeof(normalized), command);
  if(history_blank(normalized))
    return 0;

  latest = shell_history_at(history, history->count - 1);
  if(latest != 0 && strcmp(latest->command, normalized) == 0)
    return 0;

  if(history->count < SHELL_HISTORY_CAPACITY){
    index = (history->start + history->count) % SHELL_HISTORY_CAPACITY;
    history->count++;
  } else {
    index = history->start;
    history->start = (history->start + 1) % SHELL_HISTORY_CAPACITY;
  }

  entry = &history->entries[index];
  entry->number = history->next_number++;
  history_copy_command(entry->command, sizeof(entry->command), normalized);
  return 1;
}

/**
 * 重置方向键浏览状态，并清空旧草稿。
 *
 * @param cursor 待重置的浏览游标。
 */
void
shell_history_cursor_reset(struct shell_history_cursor *cursor)
{
  memset(cursor, 0, sizeof(*cursor));
  cursor->position = -1;
}

/**
 * 将文本复制到方向键替换缓冲区。
 *
 * @param dst 输出缓冲区。
 * @param dst_size 输出容量，必须大于 0。
 * @param src 待显示文本。
 */
static void
history_copy_replacement(char *dst, int dst_size, const char *src)
{
  history_copy_command(dst, dst_size, src);
}

/**
 * 向更旧的命令移动，并在首次进入浏览时保存当前草稿。
 *
 * @param history 只读会话历史。
 * @param cursor 会被更新的浏览状态。
 * @param current 首次按上键时当前尚未提交的输入。
 * @param replacement 输出应替换到命令行的文本。
 * @param replacement_size 输出容量，必须大于 0。
 * @return 有历史可展示时返回 1；空历史时返回 0。
 */
int
shell_history_cursor_up(const struct shell_history *history,
                        struct shell_history_cursor *cursor,
                        const char *current,
                        char *replacement,
                        int replacement_size)
{
  const struct shell_history_entry *entry;

  if(history->count == 0)
    return 0;
  if(!cursor->active){
    history_copy_command(cursor->draft, sizeof(cursor->draft), current);
    cursor->active = 1;
    cursor->position = history->count - 1;
  } else if(cursor->position > 0){
    cursor->position--;
  }

  entry = shell_history_at(history, cursor->position);
  history_copy_replacement(replacement, replacement_size, entry->command);
  return 1;
}

/**
 * 向更新的命令移动，越过最新项时恢复进入浏览前保存的草稿。
 *
 * @param history 只读会话历史。
 * @param cursor 会被更新的浏览状态。
 * @param replacement 输出应替换到命令行的文本。
 * @param replacement_size 输出容量，必须大于 0。
 * @return 当前处于历史浏览时返回 1；未浏览时返回 0。
 */
int
shell_history_cursor_down(const struct shell_history *history,
                          struct shell_history_cursor *cursor,
                          char *replacement,
                          int replacement_size)
{
  const struct shell_history_entry *entry;

  if(!cursor->active)
    return 0;
  if(cursor->position + 1 < history->count){
    cursor->position++;
    entry = shell_history_at(history, cursor->position);
    history_copy_replacement(replacement, replacement_size, entry->command);
  } else {
    history_copy_replacement(replacement, replacement_size, cursor->draft);
    cursor->active = 0;
    cursor->position = -1;
  }
  return 1;
}

/**
 * 标记用户已经编辑召回结果，后续上键会把当前文本视为新的草稿。
 *
 * @param cursor 待退出浏览状态的游标；保存的旧草稿不再参与当前编辑。
 */
void
shell_history_cursor_edit(struct shell_history_cursor *cursor)
{
  cursor->active = 0;
  cursor->position = -1;
}
