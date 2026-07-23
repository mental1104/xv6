#ifndef XV6_USER_HISTORY_H
#define XV6_USER_HISTORY_H

#define SHELL_HISTORY_CAPACITY 32
#define SHELL_HISTORY_COMMAND_SIZE 100

/**
 * 保存一条 Shell 会话历史记录。
 *
 * number 是不会因环形覆盖而回退的展示编号；command 保存去除行尾后的原始命令。
 */
struct shell_history_entry {
  int number;
  char command[SHELL_HISTORY_COMMAND_SIZE];
};

/**
 * 保存当前 Shell 进程拥有的固定容量历史表。
 *
 * start 指向最旧记录，count 表示有效记录数，next_number 是下一条记录的展示编号。
 */
struct shell_history {
  struct shell_history_entry entries[SHELL_HISTORY_CAPACITY];
  int start;
  int count;
  int next_number;
};

/**
 * 保存一次方向键浏览的游标与进入浏览前的未提交草稿。
 */
struct shell_history_cursor {
  int active;
  int position;
  char draft[SHELL_HISTORY_COMMAND_SIZE];
};

void shell_history_init(struct shell_history *history);
int shell_history_add(struct shell_history *history, const char *command);
int shell_history_count(const struct shell_history *history);
const struct shell_history_entry *shell_history_at(const struct shell_history *history,
                                                   int index);
void shell_history_cursor_reset(struct shell_history_cursor *cursor);
int shell_history_cursor_up(const struct shell_history *history,
                            struct shell_history_cursor *cursor,
                            const char *current,
                            char *replacement,
                            int replacement_size);
int shell_history_cursor_down(const struct shell_history *history,
                              struct shell_history_cursor *cursor,
                              char *replacement,
                              int replacement_size);
void shell_history_cursor_edit(struct shell_history_cursor *cursor);

#endif
