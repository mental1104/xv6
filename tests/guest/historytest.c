#include "kernel/types.h"
#include "user/history.h"
#include "user/user.h"

/**
 * 在断言失败时打印稳定诊断并终止测试。
 *
 * @param condition 非零表示断言成立。
 * @param message 失败时输出的场景说明。
 */
static void
check(int condition, char *message)
{
  if(condition)
    return;
  printf("historytest: %s\n", message);
  exit(1);
}

/**
 * 生成 cmdNN 形式的固定测试命令。
 *
 * @param value 1 到 99 之间的编号。
 * @param command 输出缓冲区，至少需要 6 字节。
 */
static void
make_command(int value, char *command)
{
  command[0] = 'c';
  command[1] = 'm';
  command[2] = 'd';
  command[3] = '0' + value / 10;
  command[4] = '0' + value % 10;
  command[5] = 0;
}

/**
 * 验证过滤、连续去重、环形覆盖和单调编号。
 */
static void
test_history_ring(void)
{
  struct shell_history history;
  const struct shell_history_entry *entry;
  char command[6];

  shell_history_init(&history);
  check(shell_history_add(&history, "   \t\n") == 0, "blank command recorded");
  check(shell_history_add(&history, "echo one\n") == 1, "first command rejected");
  check(shell_history_count(&history) == 1, "first command count");
  entry = shell_history_at(&history, 0);
  check(entry != 0 && entry->number == 1, "first command number");
  check(strcmp(entry->command, "echo one") == 0, "line ending not trimmed");
  check(shell_history_add(&history, "echo one") == 0, "consecutive duplicate recorded");
  check(shell_history_add(&history, "echo two") == 1, "second command rejected");
  check(shell_history_add(&history, "echo one") == 1, "non-consecutive duplicate rejected");

  shell_history_init(&history);
  for(int value = 1; value <= 33; value++){
    make_command(value, command);
    check(shell_history_add(&history, command) == 1, "ring insertion failed");
  }
  check(shell_history_count(&history) == SHELL_HISTORY_CAPACITY, "ring capacity");
  entry = shell_history_at(&history, 0);
  check(entry != 0 && entry->number == 2, "oldest number after overwrite");
  check(strcmp(entry->command, "cmd02") == 0, "oldest command after overwrite");
  entry = shell_history_at(&history, SHELL_HISTORY_CAPACITY - 1);
  check(entry != 0 && entry->number == 33, "newest number after overwrite");
  check(strcmp(entry->command, "cmd33") == 0, "newest command after overwrite");
}

/**
 * 验证上下方向浏览边界以及越过最新项后的 draft 恢复。
 */
static void
test_history_cursor(void)
{
  struct shell_history history;
  struct shell_history_cursor cursor;
  char replacement[SHELL_HISTORY_COMMAND_SIZE];

  shell_history_init(&history);
  shell_history_cursor_reset(&cursor);
  check(shell_history_cursor_up(&history, &cursor, "draft",
                                replacement, sizeof(replacement)) == 0,
        "empty history moved cursor");
  shell_history_add(&history, "one");
  shell_history_add(&history, "two");

  check(shell_history_cursor_up(&history, &cursor, "draft",
                                replacement, sizeof(replacement)) == 1,
        "first up failed");
  check(strcmp(replacement, "two") == 0, "first up not newest");
  shell_history_cursor_up(&history, &cursor, replacement,
                          replacement, sizeof(replacement));
  check(strcmp(replacement, "one") == 0, "second up not oldest");
  shell_history_cursor_up(&history, &cursor, replacement,
                          replacement, sizeof(replacement));
  check(strcmp(replacement, "one") == 0, "up crossed oldest boundary");
  shell_history_cursor_down(&history, &cursor, replacement, sizeof(replacement));
  check(strcmp(replacement, "two") == 0, "down not newer");
  shell_history_cursor_down(&history, &cursor, replacement, sizeof(replacement));
  check(strcmp(replacement, "draft") == 0, "down did not restore draft");
  check(cursor.active == 0, "cursor stayed active after draft restore");
}

/**
 * 执行全部纯历史逻辑断言。
 *
 * @return 通过 exit status 返回；成功为 0，任一断言失败为 1。
 */
int
main(void)
{
  test_history_ring();
  test_history_cursor();
  printf("historytest: OK\n");
  exit(0);
}
