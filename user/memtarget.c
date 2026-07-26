#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "user/user.h"

#define MEMTARGET_DEFAULT_PAGES 8
#define MEMTARGET_DEFAULT_INTERVAL 10
#define MEMTARGET_MAX_PAGES 64
#define MEMTARGET_MAX_INTERVAL 100000

/** 输出 memtarget 支持的页数、间隔和自动退出参数。 */
static void
usage(void)
{
  fprintf(2,
          "usage: memtarget [pages [interval_ticks]] [--exit]\n");
}

/**
 * 解析有上界的正十进制整数。
 *
 * @param text 待解析的 NUL 结尾字符串。
 * @param maximum 允许的最大值。
 * @param value 接收解析结果。
 * @return 成功返回 0；空串、非数字、零、溢出或超过上界时返回 -1。
 */
static int
parse_positive(char *text, int maximum, int *value)
{
  int parsed = 0;

  if(text[0] == 0)
    return -1;
  for(char *cursor = text; *cursor != 0; cursor++){
    if(*cursor < '0' || *cursor > '9')
      return -1;
    int digit = *cursor - '0';
    if(parsed > (maximum - digit) / 10)
      return -1;
    parsed = parsed * 10 + digit;
  }
  if(parsed <= 0 || parsed > maximum)
    return -1;

  *value = parsed;
  return 0;
}

/**
 * 逐页扩展地址空间，并把每页从 lazy 状态转换为 resident 状态。
 *
 * 默认完成后持续 sleep，便于另一进程通过 `memviz --pid` 采样稳定目标；
 * `--exit` 供自动化回归验证有限生命周期和资源回收。
 */
int
main(int argc, char **argv)
{
  int pages = MEMTARGET_DEFAULT_PAGES;
  int interval = MEMTARGET_DEFAULT_INTERVAL;
  int exit_after_complete = 0;
  int positional = 0;

  for(int i = 1; i < argc; i++){
    if(strcmp(argv[i], "--exit") == 0){
      if(exit_after_complete){
        usage();
        exit(1);
      }
      exit_after_complete = 1;
      continue;
    }

    if(positional == 0){
      if(parse_positive(argv[i], MEMTARGET_MAX_PAGES, &pages) < 0){
        usage();
        exit(1);
      }
    } else if(positional == 1){
      if(parse_positive(argv[i], MEMTARGET_MAX_INTERVAL, &interval) < 0){
        usage();
        exit(1);
      }
    } else {
      usage();
      exit(1);
    }
    positional++;
  }

  int pid = getpid();
  printf("memtarget: pid=%d pages=%d interval_ticks=%d\n",
         pid, pages, interval);

  for(int index = 0; index < pages; index++){
    char *page = sbrk(PGSIZE);
    if(page == (char *)-1){
      fprintf(2, "memtarget: sbrk failed at page %d\n", index + 1);
      exit(1);
    }

    printf("memtarget: pid=%d page=%d/%d state=lazy va=%p\n",
           pid, index + 1, pages, (uint64)page);
    sleep(interval);

    page[0] = (char)(index + 1);
    printf("memtarget: pid=%d page=%d/%d state=resident va=%p value=%d\n",
           pid, index + 1, pages, (uint64)page, page[0]);
    sleep(interval);
  }

  printf("memtarget: pid=%d state=complete pages=%d\n", pid, pages);
  if(exit_after_complete)
    exit(0);

  printf("memtarget: pid=%d state=holding; terminate with kill %d\n",
         pid, pid);
  for(;;)
    sleep(1000);
}
