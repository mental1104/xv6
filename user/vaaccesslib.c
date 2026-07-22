#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"
#include "kernel/memviz.h"
#include "user/user.h"
#include "user/vaaccesslib.h"

struct vaaccess_options {
  int op;
  int mode;
  int expect_fault;
  int expect_set;
  int snapshot;
  char *address_text;
  uint64 byte_value;
  int lazy_page;
};

struct cow_report {
  uint64 child_before_pa;
  uint64 child_after_pa;
  uint64 child_before_flags;
  uint64 child_after_flags;
  int readback;
  int query_ok;
};

/**
 * streq 比较两个 NUL 结尾字符串是否完全相同。
 *
 * @param a 左侧字符串，必须非空。
 * @param b 右侧字符串，必须非空。
 * @return 完全相同时返回 1，否则返回 0。
 */
static int
streq(char *a, char *b)
{
  return strcmp(a, b) == 0;
}

/**
 * starts_with 判断字符串是否以指定前缀开头。
 *
 * @param text 待检查字符串，必须非空。
 * @param prefix 目标前缀，必须非空。
 * @return text 以 prefix 开头时返回 1，否则返回 0。
 */
static int
starts_with(char *text, char *prefix)
{
  while(*prefix){
    if(*text != *prefix)
      return 0;
    text++;
    prefix++;
  }
  return 1;
}

/**
 * hex_digit 将一个十六进制字符转换为数值。
 *
 * @param ch 输入字符。
 * @return 0..15 表示合法十六进制数字；-1 表示非法字符。
 */
static int
hex_digit(char ch)
{
  if(ch >= '0' && ch <= '9')
    return ch - '0';
  if(ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  if(ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  return -1;
}

int
vaaccess_parse_u64(char *text, uint64 *value)
{
  int base = 10;
  uint64 result = 0;

  if(text == 0 || *text == 0 || value == 0 || *text == '-')
    return -1;
  if(text[0] == '0' && (text[1] == 'x' || text[1] == 'X')){
    base = 16;
    text += 2;
    if(*text == 0)
      return -1;
  }

  for(; *text; text++){
    int digit = base == 16 ? hex_digit(*text) :
      (*text >= '0' && *text <= '9' ? *text - '0' : -1);
    if(digit < 0 || digit >= base)
      return -1;
    if(result > (~(uint64)0 - digit) / base)
      return -1;
    result = result * base + digit;
  }

  *value = result;
  return 0;
}

/**
 * parse_byte 解析写入值并限制在单字节范围内。
 *
 * @param text 输入字符串，支持十进制或 0x 十六进制。
 * @param value 接收 0..255 的解析结果。
 * @return 成功返回 0；格式错误或超过单字节返回 -1。
 */
static int
parse_byte(char *text, uint64 *value)
{
  if(vaaccess_parse_u64(text, value) < 0)
    return -1;
  return *value <= 0xff ? 0 : -1;
}

/**
 * add_overflow 检查并执行 uint64 加法。
 *
 * @param left 左操作数。
 * @param right 右操作数。
 * @param out 接收结果。
 * @return 无溢出返回 0；溢出返回 -1。
 */
static int
add_overflow(uint64 left, uint64 right, uint64 *out)
{
  if(left > ~(uint64)0 - right)
    return -1;
  *out = left + right;
  return 0;
}

/**
 * print_byte 以稳定的两位十六进制形式打印一个字节。
 *
 * @param value 待打印字节，仅低 8 位有效。
 */
static void
print_byte(uint64 value)
{
  char *digits = "0123456789abcdef";
  printf("0x%c%c", digits[(value >> 4) & 0xf], digits[value & 0xf]);
}

/**
 * print_flags 输出 leaf PTE 的教学 flags。
 *
 * @param flags PTE_FLAGS() 的结果。
 */
static void
print_flags(uint64 flags)
{
  printf("%c%c%c%c%c%c",
         (flags & PTE_V) ? 'V' : '-',
         (flags & PTE_R) ? 'R' : '-',
         (flags & PTE_W) ? 'W' : '-',
         (flags & PTE_X) ? 'X' : '-',
         (flags & PTE_U) ? 'U' : '-',
         (flags & PTE_COW) ? 'C' : '-');
}

/**
 * snapshot_user 采集当前进程 memviz 用户视图。
 *
 * @param snapshot 接收快照，不可为空。
 * @return 成功返回 0；系统调用失败返回 -1。
 */
static int
snapshot_user(struct memviz_snapshot *snapshot)
{
  return memsnapshot(MEMVIZ_VIEW_USER, snapshot);
}

/**
 * print_query 输出目标 VA 的单页页表查询摘要。
 *
 * @param label 输出阶段标签，例如 before 或 after。
 * @param va 目标虚拟地址。
 * @param query 已由 vaquery() 填写的结果。
 */
static void
print_query(char *label, uint64 va, struct memviz_va_query *query)
{
  printf("VAACCESS %s va=%p mapped=%d", label, va, query->present);
  if(query->present){
    printf(" pte=%p flags=", query->pte);
    print_flags(query->flags);
    printf(" pa=%p kalloc_cell=%d", query->pa, query->kalloc_cell);
  }
  printf("\n");
}

/**
 * print_snapshot_block 输出访问前后的布局和目标 VA 页表路径摘要。
 *
 * @param title BEFORE 或 AFTER。
 * @param va 目标虚拟地址。
 * @param layout 当前进程用户视图快照。
 * @param query 当前进程目标 VA 查询结果。
 */
static void
print_snapshot_block(char *title, uint64 va, struct memviz_snapshot *layout,
                     struct memviz_va_query *query)
{
  printf("VAACCESS %s\n", title);
  printf("VAACCESS layout sz=%p stack=[%p,%p) guard=%p limit=%p free_pages=%d\n",
         layout->process_size, layout->stack_bottom, layout->stack_top,
         layout->stack_guard_start, layout->user_limit, (int)layout->free_pages);
  print_query(title, va, query);
  for(int i = 0; i < 3; i++){
    struct memviz_pte_level *level = &query->levels[i];
    printf("VAACCESS %s L%d[%d] present=%d pte=%p pa=%p flags=",
           title, level->level, level->index, level->present,
           level->pte, level->pa);
    print_flags(level->flags);
    printf("\n");
  }
}

/**
 * resolve_address 解析相对于当前进程快照的地址表达式。
 *
 * @param text 地址表达式，支持绝对数值和 image/guard/stack/heap/brk/limit。
 * @param layout 当前进程的用户视图快照。
 * @param va 接收最终绝对 VA。
 * @param reserved_bytes 接收本函数因 heap+N 临时 sbrk 的字节数。
 * @return 成功返回 0；格式、溢出、越界或 sbrk 失败返回 -1。
 */
static int
resolve_address(char *text, struct memviz_snapshot *layout, uint64 *va,
                int *reserved_bytes)
{
  uint64 offset;
  *reserved_bytes = 0;

  if(vaaccess_parse_u64(text, va) == 0)
    return *va < MAXVA ? 0 : -1;

  if(starts_with(text, "image+")){
    if(vaaccess_parse_u64(text + 6, &offset) < 0)
      return -1;
    return add_overflow(layout->image_start, offset, va) < 0 || *va >= MAXVA ?
      -1 : 0;
  }
  if(starts_with(text, "guard+")){
    if(vaaccess_parse_u64(text + 6, &offset) < 0)
      return -1;
    return add_overflow(layout->stack_guard_start, offset, va) < 0 || *va >= MAXVA ?
      -1 : 0;
  }
  if(starts_with(text, "stack-")){
    if(vaaccess_parse_u64(text + 6, &offset) < 0 || layout->stack_top < offset)
      return -1;
    *va = layout->stack_top - offset;
    return *va < MAXVA ? 0 : -1;
  }
  if(starts_with(text, "brk+")){
    if(vaaccess_parse_u64(text + 4, &offset) < 0)
      return -1;
    return add_overflow(layout->process_size, offset, va) < 0 || *va >= MAXVA ?
      -1 : 0;
  }
  if(starts_with(text, "limit-")){
    if(vaaccess_parse_u64(text + 6, &offset) < 0 || layout->user_limit < offset)
      return -1;
    *va = layout->user_limit - offset;
    return *va < MAXVA ? 0 : -1;
  }
  if(starts_with(text, "heap+")){
    if(vaaccess_parse_u64(text + 5, &offset) < 0)
      return -1;
    uint64 reserve = PGROUNDUP(offset + 1);
    if(reserve == 0 || reserve > 16 * PGSIZE)
      return -1;
    char *base = sbrk((int)reserve);
    if(base == (char *)-1)
      return -1;
    *reserved_bytes = (int)reserve;
    return add_overflow((uint64)base, offset, va) < 0 || *va >= MAXVA ?
      -1 : 0;
  }

  return -1;
}

/**
 * resolve_probe_address 解析 vaprobe 会话内地址表达式。
 *
 * @param text 地址表达式；heap+N 绑定到 reserve 记录的 heap_base。
 * @param layout 当前会话进程快照。
 * @param heap_base 最近一次从 0 页进入 reserve 状态时的旧 break。
 * @param va 接收最终绝对 VA。
 * @return 成功返回 0；没有 reserve 却使用 heap+N 或解析失败返回 -1。
 */
static int
resolve_probe_address(char *text, struct memviz_snapshot *layout,
                      uint64 heap_base, uint64 *va)
{
  uint64 offset;
  int ignored;

  if(starts_with(text, "heap+")){
    if(heap_base == 0 || vaaccess_parse_u64(text + 5, &offset) < 0)
      return -1;
    return add_overflow(heap_base, offset, va) < 0 || *va >= MAXVA ? -1 : 0;
  }
  return resolve_address(text, layout, va, &ignored);
}

/**
 * parse_expect 解析 --expect 参数。
 *
 * @param value ok 或 fault。
 * @param options 待更新选项。
 * @return 成功返回 0；未知期望返回 -1。
 */
static int
parse_expect(char *value, struct vaaccess_options *options)
{
  if(streq(value, "ok")){
    options->expect_set = 1;
    options->expect_fault = 0;
    return 0;
  }
  if(streq(value, "fault")){
    options->expect_set = 1;
    options->expect_fault = 1;
    return 0;
  }
  return -1;
}

/**
 * worker_access 在当前进程中解析地址并执行真正的 volatile load/store。
 *
 * @param options 已解析命令选项。
 * @return 成功访问返回 0；参数或观测失败返回 2；非法 VA fault 会直接杀死进程。
 */
static int
worker_access(struct vaaccess_options *options)
{
  static struct memviz_snapshot before_layout;
  static struct memviz_snapshot after_layout;
  struct memviz_va_query before_query;
  struct memviz_va_query after_query;
  uint64 va = 0;
  int reserved_bytes = 0;

  if(snapshot_user(&before_layout) < 0)
    return 2;

  if(options->mode == VAACCESS_MODE_LAZY){
    int pages = options->lazy_page + 1;
    char *base = sbrk(pages * PGSIZE);
    if(base == (char *)-1)
      return 2;
    reserved_bytes = pages * PGSIZE;
    va = (uint64)base + (uint64)options->lazy_page * PGSIZE;
    printf("VAACCESS mode=lazy\n");
    printf("VAACCESS reserved_pages=%d\n", pages);
    printf("VAACCESS target_page=%d\n", options->lazy_page);
    if(snapshot_user(&before_layout) < 0)
      return 2;
  } else if(resolve_address(options->address_text, &before_layout, &va,
                            &reserved_bytes) < 0){
    printf("VAACCESS diagnostic parse-address failed\n");
    return 2;
  }

  if(vaquery(va, &before_query) < 0)
    return 2;

  printf("VAACCESS begin op=%s va=%p",
         options->op == VAACCESS_READ ? "read" : "write", va);
  if(options->op == VAACCESS_WRITE){
    printf(" value=");
    print_byte(options->byte_value);
  }
  printf("\n");

  printf("VAACCESS before mapped=%d free_pages=%d\n",
         before_query.present, (int)before_layout.free_pages);
  if(options->snapshot)
    print_snapshot_block("BEFORE", va, &before_layout, &before_query);

  if(options->op == VAACCESS_READ){
    volatile unsigned char value = *(volatile unsigned char *)va;
    printf("VAACCESS value=");
    print_byte(value);
    printf("\n");
  } else {
    *(volatile unsigned char *)va = (unsigned char)options->byte_value;
    volatile unsigned char readback = *(volatile unsigned char *)va;
    printf("VAACCESS readback=");
    print_byte(readback);
    printf("\n");
  }

  if(snapshot_user(&after_layout) < 0 || vaquery(va, &after_query) < 0)
    return 2;
  printf("VAACCESS after mapped=%d free_pages=%d\n",
         after_query.present, (int)after_layout.free_pages);
  if(options->snapshot)
    print_snapshot_block("AFTER", va, &after_layout, &after_query);

  if(options->mode == VAACCESS_MODE_LAZY && before_query.present)
    return 1;
  if(options->mode == VAACCESS_MODE_LAZY && !after_query.present)
    return 1;

  if(reserved_bytes > 0 && sbrk(-reserved_bytes) == (char *)-1)
    return 1;
  return 0;
}

/**
 * run_supervised 在子进程中运行真实访问，并把 wait status 归一化。
 *
 * @param options 已解析命令选项。
 * @return 结果符合 --expect 时返回 0；不符合返回 1；参数错误返回 2。
 */
static int
run_supervised(int argc, char **argv, struct vaaccess_options *options)
{
  int pid = fork();
  int status = 0;

  if(pid < 0){
    printf("VAACCESS diagnostic fork failed\n");
    return 1;
  }
  if(pid == 0){
    char *worker_argv[16];
    if(argc + 1 >= 16)
      exit(2);
    worker_argv[0] = argv[0];
    worker_argv[1] = "--worker";
    for(int i = 1; i < argc; i++)
      worker_argv[i + 1] = argv[i];
    worker_argv[argc + 1] = 0;
    exec(argv[0], worker_argv);
    exit(2);
  }

  if(wait(&status) != pid){
    printf("VAACCESS diagnostic wait failed\n");
    return 1;
  }

  int actual_fault = status == -1;
  int actual_ok = status == 0;
  if(actual_fault)
    printf("VAACCESS result=fault\n");
  else if(actual_ok)
    printf("VAACCESS result=ok\n");
  else {
    printf("VAACCESS result=error\n");
    printf("VAACCESS worker_status=%d\n", status);
    printf("VAACCESS done status=2\n");
    return 2;
  }

  if(actual_fault)
    printf("VAACCESS worker_status=-1\n");

  int expected_fault = options->expect_set ? options->expect_fault : 0;
  int done = actual_fault == expected_fault ? 0 : 1;
  printf("VAACCESS done status=%d\n", done);
  return done;
}

/**
 * parse_common_options 解析 read/write 命令共享选项。
 *
 * @param argc 参数数量。
 * @param argv 参数数组。
 * @param options 接收解析结果。
 * @return 成功返回 0；参数错误返回 -1。
 */
static int
parse_common_options(int argc, char **argv, struct vaaccess_options *options)
{
  int op = options->op;
  memset(options, 0, sizeof(*options));
  options->op = op;
  options->mode = VAACCESS_MODE_DIRECT;

  int i = 1;
  if(i < argc && streq(argv[i], "--lazy")){
    uint64 page;
    if(i + 1 >= argc || vaaccess_parse_u64(argv[i + 1], &page) < 0 ||
       page > 15)
      return -1;
    options->mode = VAACCESS_MODE_LAZY;
    options->lazy_page = (int)page;
    i += 2;
  } else if(options->op == VAACCESS_WRITE && i < argc && streq(argv[i], "--cow")){
    options->mode = VAACCESS_MODE_COW;
    i++;
  } else {
    if(i >= argc)
      return -1;
    options->address_text = argv[i++];
  }

  if(options->op == VAACCESS_WRITE){
    if(i >= argc || parse_byte(argv[i], &options->byte_value) < 0)
      return -1;
    i++;
  }

  while(i < argc){
    if(streq(argv[i], "--snapshot")){
      options->snapshot = 1;
      i++;
    } else if(streq(argv[i], "--expect")){
      if(i + 1 >= argc || parse_expect(argv[i + 1], options) < 0)
        return -1;
      i += 2;
    } else {
      return -1;
    }
  }

  return 0;
}

/**
 * run_cow 执行明确的 COW 写入实验。
 *
 * @param options 写命令选项；byte_value 是 child 写入值。
 * @return 验证通过返回 0；任一不变量失败返回 1 或 2。
 */
static int
run_cow(struct vaaccess_options *options)
{
  static struct memviz_snapshot before;
  static struct memviz_snapshot after_wait;
  static struct memviz_snapshot after_shrink;
  struct memviz_va_query parent_cow;
  struct cow_report report;
  int start_pipe[2];
  int report_pipe[2];
  char token = 'x';
  char *base;
  int pid;
  int status = 0;
  unsigned char initial = 0x31;

  memset(&report, 0, sizeof(report));
  if(snapshot_user(&before) < 0)
    return 2;
  base = sbrk(PGSIZE);
  if(base == (char *)-1)
    return 2;
  *(volatile unsigned char *)base = initial;

  if(pipe(start_pipe) < 0 || pipe(report_pipe) < 0)
    return 2;
  pid = fork();
  if(pid < 0)
    return 2;
  if(pid == 0){
    close(start_pipe[1]);
    close(report_pipe[0]);
    if(read(start_pipe[0], &token, 1) != 1)
      exit(2);
    struct memviz_va_query child_before;
    struct memviz_va_query child_after;
    if(vaquery((uint64)base, &child_before) < 0)
      exit(2);
    *(volatile unsigned char *)base = (unsigned char)options->byte_value;
    volatile unsigned char readback = *(volatile unsigned char *)base;
    if(vaquery((uint64)base, &child_after) < 0)
      exit(2);
    report.child_before_pa = child_before.pa;
    report.child_after_pa = child_after.pa;
    report.child_before_flags = child_before.flags;
    report.child_after_flags = child_after.flags;
    report.readback = readback;
    report.query_ok = child_before.present && child_after.present;
    write(report_pipe[1], &report, sizeof(report));
    exit(0);
  }

  close(start_pipe[0]);
  close(report_pipe[1]);
  if(vaquery((uint64)base, &parent_cow) < 0)
    return 2;
  write(start_pipe[1], &token, 1);
  close(start_pipe[1]);
  if(read(report_pipe[0], &report, sizeof(report)) != sizeof(report))
    return 2;
  close(report_pipe[0]);
  if(wait(&status) != pid)
    return 2;

  volatile unsigned char parent_value = *(volatile unsigned char *)base;
  snapshot_user(&after_wait);
  int ok = status == 0 && report.query_ok &&
    report.child_before_pa == parent_cow.pa &&
    report.child_after_pa != parent_cow.pa &&
    (parent_cow.flags & PTE_COW) && (report.child_before_flags & PTE_COW) &&
    (report.child_before_flags & PTE_W) == 0 &&
    (report.child_after_flags & PTE_W) &&
    (report.child_after_flags & PTE_COW) == 0 &&
    report.readback == (int)options->byte_value &&
    parent_value == initial;

  printf("VAACCESS mode=cow\n");
  printf("VAACCESS begin op=write va=%p value=", (uint64)base);
  print_byte(options->byte_value);
  printf("\n");
  if(options->snapshot){
    printf("VAACCESS BEFORE\n");
    print_query("parent-cow", (uint64)base, &parent_cow);
    printf("VAACCESS AFTER\n");
    printf("VAACCESS child before_pa=%p after_pa=%p before_flags=",
           report.child_before_pa, report.child_after_pa);
    print_flags(report.child_before_flags);
    printf(" after_flags=");
    print_flags(report.child_after_flags);
    printf("\n");
    printf("VAACCESS free_pages before=%d after_child=%d\n",
           (int)before.free_pages, (int)after_wait.free_pages);
  }
  printf("VAACCESS readback=");
  print_byte(report.readback);
  printf("\n");
  printf("VAACCESS parent_value=");
  print_byte(parent_value);
  printf("\n");

  if(sbrk(-PGSIZE) == (char *)-1)
    ok = 0;
  snapshot_user(&after_shrink);
  if(after_shrink.free_pages != before.free_pages)
    ok = 0;

  printf("VAACCESS result=%s\n", ok ? "ok" : "error");
  printf("VAACCESS done status=%d\n", ok ? 0 : 1);
  return ok ? 0 : 1;
}

int
vaaccess_read_main(int argc, char **argv)
{
  struct vaaccess_options options;
  if(argc >= 2 && streq(argv[1], "--worker")){
    char *worker_argv[16];
    if(argc > 16)
      return 2;
    worker_argv[0] = argv[0];
    for(int i = 2; i < argc; i++)
      worker_argv[i - 1] = argv[i];
    worker_argv[argc - 1] = 0;
    options.op = VAACCESS_READ;
    if(parse_common_options(argc - 1, worker_argv, &options) < 0 ||
       options.mode == VAACCESS_MODE_COW)
      return 2;
    options.op = VAACCESS_READ;
    return worker_access(&options);
  }

  options.op = VAACCESS_READ;
  if(parse_common_options(argc, argv, &options) < 0 || options.mode == VAACCESS_MODE_COW){
    fprintf(2, "usage: varead <address> [--expect ok|fault] [--snapshot]\n");
    fprintf(2, "       varead --lazy <page-offset> [--expect ok|fault] [--snapshot]\n");
    return 2;
  }
  options.op = VAACCESS_READ;
  return run_supervised(argc, argv, &options);
}

int
vaaccess_write_main(int argc, char **argv)
{
  struct vaaccess_options options;
  if(argc >= 2 && streq(argv[1], "--worker")){
    char *worker_argv[16];
    if(argc > 16)
      return 2;
    worker_argv[0] = argv[0];
    for(int i = 2; i < argc; i++)
      worker_argv[i - 1] = argv[i];
    worker_argv[argc - 1] = 0;
    options.op = VAACCESS_WRITE;
    if(parse_common_options(argc - 1, worker_argv, &options) < 0 ||
       options.mode == VAACCESS_MODE_COW)
      return 2;
    options.op = VAACCESS_WRITE;
    return worker_access(&options);
  }

  options.op = VAACCESS_WRITE;
  if(parse_common_options(argc, argv, &options) < 0){
    fprintf(2, "usage: vawrite <address> <byte> [--expect ok|fault] [--snapshot]\n");
    fprintf(2, "       vawrite --lazy <page-offset> <byte> [--expect ok|fault] [--snapshot]\n");
    fprintf(2, "       vawrite --cow <byte> [--snapshot]\n");
    return 2;
  }
  options.op = VAACCESS_WRITE;
  if(options.mode == VAACCESS_MODE_COW)
    return run_cow(&options);
  return run_supervised(argc, argv, &options);
}

/**
 * trim_newline 删除 gets() 留下的换行符。
 *
 * @param text 可修改字符串。
 */
static void
trim_newline(char *text)
{
  int n = strlen(text);
  if(n > 0 && text[n - 1] == '\n')
    text[n - 1] = 0;
}

/**
 * split_line 将交互命令切成最多四个参数。
 *
 * @param line 可修改命令行；空白会被替换为 NUL。
 * @param argv 接收参数指针。
 * @param max 最多接收多少参数。
 * @return 实际参数数量。
 */
static int
split_line(char *line, char **argv, int max)
{
  int argc = 0;
  char *p = line;
  while(*p && argc < max){
    while(*p == ' ' || *p == '\t')
      p++;
    if(*p == 0)
      break;
    argv[argc++] = p;
    while(*p && *p != ' ' && *p != '\t')
      p++;
    if(*p){
      *p = 0;
      p++;
    }
  }
  return argc;
}

/**
 * probe_access 在 vaprobe 当前进程中直接执行一次读写。
 *
 * @param op VAACCESS_READ 或 VAACCESS_WRITE。
 * @param address_text vaprobe 地址表达式。
 * @param byte_value 写操作的目标字节；读操作忽略。
 * @param heap_base reserve 命令记录的会话 heap 起点。
 * @return 成功访问返回 0；解析或查询失败返回 -1；非法访问会杀死 vaprobe。
 */
static int
probe_access(int op, char *address_text, uint64 byte_value, uint64 heap_base)
{
  struct memviz_va_query before_query;
  struct memviz_va_query after_query;
  static struct memviz_snapshot before_layout;
  static struct memviz_snapshot after_layout;
  uint64 va;

  if(snapshot_user(&before_layout) < 0 ||
     resolve_probe_address(address_text, &before_layout, heap_base, &va) < 0 ||
     vaquery(va, &before_query) < 0)
    return -1;

  printf("VAACCESS begin op=%s va=%p", op == VAACCESS_READ ? "read" : "write", va);
  if(op == VAACCESS_WRITE){
    printf(" value=");
    print_byte(byte_value);
  }
  printf("\n");
  printf("VAACCESS before mapped=%d free_pages=%d\n",
         before_query.present, (int)before_layout.free_pages);

  if(op == VAACCESS_READ){
    volatile unsigned char value = *(volatile unsigned char *)va;
    printf("VAACCESS value=");
    print_byte(value);
    printf("\n");
  } else {
    *(volatile unsigned char *)va = (unsigned char)byte_value;
    volatile unsigned char readback = *(volatile unsigned char *)va;
    printf("VAACCESS readback=");
    print_byte(readback);
    printf("\n");
  }

  if(snapshot_user(&after_layout) < 0 || vaquery(va, &after_query) < 0)
    return -1;
  printf("VAACCESS after mapped=%d free_pages=%d\n",
         after_query.present, (int)after_layout.free_pages);
  return 0;
}

int
vaaccess_probe_main(int argc, char **argv)
{
  char line[128];
  char *parts[4];
  int reserved_pages = 0;
  uint64 heap_base = 0;

  (void)argv;
  if(argc != 1){
    fprintf(2, "usage: vaprobe\n");
    return 2;
  }

  for(;;){
    printf("va> ");
    gets(line, sizeof(line));
    trim_newline(line);
    int n = split_line(line, parts, 4);
    if(n == 0)
      continue;
    if(streq(parts[0], "quit"))
      break;
    if(streq(parts[0], "help")){
      printf("commands: help snapshot pte reserve shrink read write cow quit\n");
    } else if(streq(parts[0], "snapshot")){
      static struct memviz_snapshot snap;
      if(snapshot_user(&snap) == 0)
        printf("VAACCESS snapshot sz=%p stack=[%p,%p) guard=%p free_pages=%d\n",
               snap.process_size, snap.stack_bottom, snap.stack_top,
               snap.stack_guard_start, (int)snap.free_pages);
      else
        printf("VAACCESS error snapshot\n");
    } else if(streq(parts[0], "reserve") && n == 2){
      uint64 pages;
      char *base;
      if(vaaccess_parse_u64(parts[1], &pages) == 0 && pages <= 16 &&
         (base = sbrk((int)pages * PGSIZE)) != (char *)-1){
        if(reserved_pages == 0)
          heap_base = (uint64)base;
        reserved_pages += (int)pages;
        printf("VAACCESS reserved_pages=%d\n", reserved_pages);
      } else {
        printf("VAACCESS error reserve\n");
      }
    } else if(streq(parts[0], "shrink") && n == 2){
      uint64 pages;
      if(vaaccess_parse_u64(parts[1], &pages) == 0 && pages <= (uint64)reserved_pages &&
         sbrk(-((int)pages * PGSIZE)) != (char *)-1){
        reserved_pages -= (int)pages;
        if(reserved_pages == 0)
          heap_base = 0;
        printf("VAACCESS reserved_pages=%d\n", reserved_pages);
      } else {
        printf("VAACCESS error shrink\n");
      }
    } else if(streq(parts[0], "pte") && n == 2){
      static struct memviz_snapshot snap;
      struct memviz_va_query query;
      uint64 va;
      if(snapshot_user(&snap) == 0 &&
         resolve_probe_address(parts[1], &snap, heap_base, &va) == 0 &&
         vaquery(va, &query) == 0)
        print_query("pte", va, &query);
      else
        printf("VAACCESS error pte\n");
    } else if(streq(parts[0], "read") && n == 2){
      if(probe_access(VAACCESS_READ, parts[1], 0, heap_base) < 0)
        printf("VAACCESS error read\n");
    } else if(streq(parts[0], "write") && n == 3){
      uint64 byte_value;
      if(parse_byte(parts[2], &byte_value) == 0 &&
         probe_access(VAACCESS_WRITE, parts[1], byte_value, heap_base) == 0){
      } else {
        printf("VAACCESS error write\n");
      }
    } else if(streq(parts[0], "cow") && n == 2){
      char *args[] = {"vaprobe-cow", "--cow", parts[1], "--snapshot", 0};
      struct vaaccess_options opts;
      opts.op = VAACCESS_WRITE;
      if(parse_common_options(4, args, &opts) == 0)
        run_cow(&opts);
      else
        printf("VAACCESS error cow\n");
    } else {
      printf("VAACCESS error unknown-command\n");
    }
  }

  return 0;
}
