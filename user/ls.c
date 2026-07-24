#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/fs.h"

#define LS_PATH_SIZE 512
#define LS_SIZE_SIZE 32
#define LS_TIME_SIZE 16

/** 保存一次 ls 调用启用的展示选项。 */
struct ls_options {
  int show_all;
  int long_format;
  int human_readable;
  int show_help;
};

/**
 * 描述一种 inode 类型的 Linux 风格 mode 占位和名称样式。
 *
 * xv6 尚无权限、UID/GID 和 LS_COLORS；新增类型时只需扩展此表，而不必在
 * 紧凑输出和长格式输出中分别增加条件分支。
 */
struct entry_format {
  short type;
  char *mode;
  char *prefix;
  char *suffix;
};

static struct entry_format entry_formats[] = {
  {T_DIR,     "drwxr-xr-x", "\033[1;34m", "\033[0m"},
  {T_FILE,    "-rw-r--r--", "", ""},
  {T_DEVICE,  "crw-rw-rw-", "", ""},
  {T_SYMLINK, "lrwxrwxrwx", "", ""},
};

static struct entry_format unknown_format = {
  0, "?---------", "", ""
};

/** 输出当前实现支持的命令行形式和长格式字段。 */
static void
usage(void)
{
  printf("Usage: ls [-alh] [--help] [--] [FILE ...]\n");
  printf("  -a  show entries starting with .\n");
  printf("  -l  long format: MODE NLINK OWNER GROUP SIZE MTIME NAME\n");
  printf("  -h  human-readable sizes with -l\n");
}

/**
 * 解析 -a、-l、-h、--help 和 --，并定位首个路径参数。
 *
 * @param argc 命令行参数数量。
 * @param argv 命令行参数数组。
 * @param options 输出选项集合，调用前无需初始化。
 * @param first_path 输出首个路径参数下标；没有路径时等于 argc。
 * @return 参数合法时返回 0；遇到未知选项时返回 -1，并已输出诊断。
 */
static int
parse_options(int argc, char **argv, struct ls_options *options, int *first_path)
{
  int i;
  int j;

  memset(options, 0, sizeof(*options));
  for(i = 1; i < argc; i++){
    if(strcmp(argv[i], "--") == 0){
      i++;
      break;
    }
    if(strcmp(argv[i], "--help") == 0){
      options->show_help = 1;
      i++;
      break;
    }
    if(argv[i][0] != '-' || argv[i][1] == 0)
      break;
    if(argv[i][1] == '-'){
      fprintf(2, "ls: unrecognized option '%s'\n", argv[i]);
      usage();
      return -1;
    }

    for(j = 1; argv[i][j] != 0; j++){
      switch(argv[i][j]){
      case 'a':
        options->show_all = 1;
        break;
      case 'l':
        options->long_format = 1;
        break;
      case 'h':
        options->human_readable = 1;
        break;
      default:
        fprintf(2, "ls: invalid option -- '%c'\n", argv[i][j]);
        usage();
        return -1;
      }
    }
  }

  *first_path = i;
  return 0;
}

/**
 * 从路径中提取用于显示的最后一个名称组件。
 *
 * @param path 输入路径，不会被修改。
 * @param name 输出缓冲区，至少需要 DIRSIZ + 2 字节；过长名称按 xv6 的
 *             DIRSIZ 上限截断。
 * @return name 缓冲区首地址。
 */
static char*
display_name(char *path, char *name)
{
  char *start;
  char *end;
  int length;

  end = path + strlen(path);
  while(end > path + 1 && end[-1] == '/')
    end--;
  start = end;
  while(start > path && start[-1] != '/')
    start--;

  length = end - start;
  if(length == 0 && path[0] == '/'){
    name[0] = '/';
    name[1] = 0;
    return name;
  }
  if(length > DIRSIZ)
    length = DIRSIZ;
  memmove(name, start, length);
  name[length] = 0;
  return name;
}

/**
 * 将固定宽度 dirent 名称转换为以 NUL 结尾的用户态字符串。
 *
 * @param entry_name 磁盘目录项中的 DIRSIZ 字节名称。
 * @param name 输出缓冲区，至少需要 DIRSIZ + 1 字节。
 */
static void
copy_dirent_name(char *entry_name, char *name)
{
  int length;

  for(length = 0; length < DIRSIZ && entry_name[length] != 0; length++)
    name[length] = entry_name[length];
  name[length] = 0;
}

/**
 * 判断名称是否属于默认应隐藏的点条目。
 *
 * @param name 以 NUL 结尾的单个名称组件。
 * @return 名称以 '.' 开头时返回 1，否则返回 0。
 */
static int
is_hidden(char *name)
{
  return name[0] == '.';
}

/**
 * 查找 inode 类型对应的 mode 和 ANSI 名称样式。
 *
 * @param type struct stat 中的 T_* 类型。
 * @return 指向静态格式项的指针；未知类型返回固定后备格式。
 */
static struct entry_format*
find_entry_format(short type)
{
  int i;

  for(i = 0; i < sizeof(entry_formats) / sizeof(entry_formats[0]); i++){
    if(entry_formats[i].type == type)
      return &entry_formats[i];
  }
  return &unknown_format;
}

/**
 * 输出带类型样式的名称，并立即复位 ANSI 状态以免污染 shell 提示符。
 *
 * @param name 待展示名称。
 * @param type inode 类型。
 */
static void
print_styled_name(char *name, short type)
{
  struct entry_format *format = find_entry_format(type);

  printf("%s%s%s", format->prefix, name, format->suffix);
}

/**
 * 将无符号整数转换为十进制字符串。
 *
 * @param value 待格式化数值。
 * @param buffer 输出缓冲区，至少需要 21 字节。
 */
static void
format_uint64(uint64 value, char *buffer)
{
  char reverse[21];
  int length;
  int i;

  length = 0;
  do {
    reverse[length++] = '0' + value % 10;
    value /= 10;
  } while(value != 0);

  for(i = 0; i < length; i++)
    buffer[i] = reverse[length - i - 1];
  buffer[length] = 0;
}

/**
 * 使用 1024 进制单位格式化文件大小，不依赖浮点运行时。
 *
 * @param size 原始字节数。
 * @param buffer 输出缓冲区，至少需要 LS_SIZE_SIZE 字节。
 */
static void
format_human_size(uint64 size, char *buffer)
{
  static char units[] = "KMGT";
  uint64 divisor;
  uint64 whole;
  uint64 decimal;
  int unit;
  int length;

  if(size < 1024){
    format_uint64(size, buffer);
    return;
  }

  divisor = 1024;
  unit = 0;
  while(unit < 3 && size >= divisor * 1024){
    divisor *= 1024;
    unit++;
  }

  whole = size / divisor;
  decimal = ((size % divisor) * 10 + divisor / 2) / divisor;
  if(decimal == 10){
    whole++;
    decimal = 0;
  }

  format_uint64(whole, buffer);
  length = strlen(buffer);
  if(whole < 10 && decimal != 0){
    buffer[length++] = '.';
    buffer[length++] = '0' + decimal;
  }
  buffer[length++] = units[unit];
  buffer[length] = 0;
}

/**
 * 按当前选项格式化文件大小。
 *
 * @param size 原始字节数。
 * @param human_readable 非零时使用 K/M/G/T 单位，否则输出字节数。
 * @param buffer 输出缓冲区，至少需要 LS_SIZE_SIZE 字节。
 */
static void
format_size(uint64 size, int human_readable, char *buffer)
{
  if(human_readable)
    format_human_size(size, buffer);
  else
    format_uint64(size, buffer);
}

/**
 * 判断公历年份是否为闰年。
 *
 * @param year 完整年份。
 * @return 闰年返回 1，否则返回 0。
 */
static int
is_leap_year(uint year)
{
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

/**
 * 返回指定公历月份的天数。
 *
 * @param year 完整年份。
 * @param month 从 0 开始的月份下标。
 * @return 该月天数。
 */
static int
days_in_month(uint year, int month)
{
  static char month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  if(month == 1 && is_leap_year(year))
    return 29;
  return month_days[month];
}

/**
 * 将 32 位 Unix 秒数格式化为固定宽度 UTC 时间。
 *
 * @param timestamp 自 1970-01-01 起的秒数。
 * @param buffer 输出 `Mon DD HH:MM`，至少需要 LS_TIME_SIZE 字节。
 */
static void
format_mtime(uint timestamp, char *buffer)
{
  static char *months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  uint64 days = timestamp / 86400;
  uint seconds = timestamp % 86400;
  uint year = 1970;
  int month = 0;
  int day;
  int hour;
  int minute;

  while(days >= (uint64)(is_leap_year(year) ? 366 : 365)){
    days -= is_leap_year(year) ? 366 : 365;
    year++;
  }
  while(days >= (uint64)days_in_month(year, month)){
    days -= days_in_month(year, month);
    month++;
  }

  day = days + 1;
  hour = seconds / 3600;
  minute = (seconds % 3600) / 60;
  memmove(buffer, months[month], 3);
  buffer[3] = ' ';
  buffer[4] = day >= 10 ? '0' + day / 10 : ' ';
  buffer[5] = '0' + day % 10;
  buffer[6] = ' ';
  buffer[7] = '0' + hour / 10;
  buffer[8] = '0' + hour % 10;
  buffer[9] = ':';
  buffer[10] = '0' + minute / 10;
  buffer[11] = '0' + minute % 10;
  buffer[12] = 0;
}

/**
 * 在字段内容前输出空格，使其至少达到指定宽度。
 *
 * @param text 待输出字符串。
 * @param width 最小字段宽度，小于字符串长度时不截断。
 */
static void
print_right_aligned(char *text, int width)
{
  int padding;

  padding = width - strlen(text);
  while(padding-- > 0)
    printf(" ");
  printf("%s", text);
}

/**
 * 按 Linux 风格字段顺序输出一个长格式条目。
 *
 * 权限和 root owner/group 是 xv6 尚未实现对应模型时的稳定展示占位；mtime、
 * nlink 和 size 来自真实 inode 元数据。
 *
 * @param name 只用于展示的名称。
 * @param st 通过 O_NOFOLLOW 获取的真实 xv6 元数据。
 * @param options 当前展示选项。
 */
static void
print_long_entry(char *name, struct stat *st, struct ls_options *options)
{
  char nlink[LS_SIZE_SIZE];
  char size[LS_SIZE_SIZE];
  char mtime[LS_TIME_SIZE];
  struct entry_format *format = find_entry_format(st->type);

  format_uint64(st->nlink, nlink);
  format_size(st->size, options->human_readable, size);
  format_mtime(st->mtime, mtime);
  printf("%s ", format->mode);
  print_right_aligned(nlink, 3);
  printf(" root root ");
  print_right_aligned(size, 6);
  printf(" %s ", mtime);
  print_styled_name(name, st->type);
  printf("\n");
}

/**
 * 优先读取路径本身的 stat，在当前内核拒绝给目录附加 O_NOFOLLOW 时回退。
 *
 * xv6 的 can_open_inode_locked() 会把 O_NOFOLLOW 误当成目录写入模式，因此真实
 * 目录第一次打开会失败。符号链接本身可以用 O_NOFOLLOW 打开，只有失败后才以
 * O_RDONLY 重试；这样既保留符号链接类型，也不需要为了 ls 修改内核 open 语义。
 *
 * @param path 待读取路径。
 * @param st 输出元数据。
 * @return 成功返回 0；两种打开方式或 fstat 均失败时返回 -1，临时 fd 已关闭。
 */
static int
stat_no_follow(char *path, struct stat *st)
{
  int fd;

  fd = open(path, O_RDONLY | O_NOFOLLOW);
  if(fd < 0)
    fd = open(path, O_RDONLY);
  if(fd < 0)
    return -1;
  if(fstat(fd, st) < 0){
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

/**
 * 安全拼接目录路径和单个 dirent 名称。
 *
 * @param directory 父目录路径。
 * @param name 单个名称组件。
 * @param path 输出完整路径。
 * @param capacity path 缓冲区容量。
 * @return 拼接成功返回 0；超出容量返回 -1，path 内容不可使用。
 */
static int
join_path(char *directory, char *name, char *path, int capacity)
{
  int directory_length;
  int name_length;
  int need_slash;
  int length;

  directory_length = strlen(directory);
  name_length = strlen(name);
  need_slash = directory_length > 0 && directory[directory_length - 1] != '/';
  length = directory_length + need_slash + name_length + 1;
  if(length > capacity)
    return -1;

  memmove(path, directory, directory_length);
  length = directory_length;
  if(need_slash)
    path[length++] = '/';
  memmove(path + length, name, name_length);
  path[length + name_length] = 0;
  return 0;
}

/**
 * 将一个原始 dirent 解析为可展示名称、完整路径和 stat。
 *
 * @param directory 父目录路径。
 * @param entry 当前磁盘目录项。
 * @param options 当前展示选项。
 * @param name 输出名称缓冲区，容量至少 DIRSIZ + 1。
 * @param full_path 输出完整路径缓冲区。
 * @param st 输出 inode 元数据。
 * @param report_errors 非零时输出路径过长或 stat 失败诊断。
 * @return 1 表示可展示；0 表示空目录项或被隐藏；-1 表示处理失败。
 */
static int
load_directory_entry(char *directory, struct dirent *entry,
                     struct ls_options *options, char *name, char *full_path,
                     struct stat *st, int report_errors)
{
  if(entry->inum == 0)
    return 0;
  copy_dirent_name(entry->name, name);
  if(!options->show_all && is_hidden(name))
    return 0;
  if(join_path(directory, name, full_path, LS_PATH_SIZE) < 0){
    if(report_errors)
      fprintf(2, "ls: path too long: '%s/%s'\n", directory, name);
    return -1;
  }
  if(stat_no_follow(full_path, st) < 0){
    if(report_errors)
      fprintf(2, "ls: cannot stat '%s'\n", full_path);
    return -1;
  }
  return 1;
}

/**
 * 汇总当前选项实际展示条目的 apparent size。
 *
 * xv6 尚未向 stat 暴露已分配块数，因此该值不是 GNU ls 的 st_blocks 汇总；
 * 稀疏文件和目录会体现这一教学边界。
 *
 * @param path 目录路径。
 * @param options 当前展示选项。
 * @param total 输出字节总数。
 * @return 扫描成功返回 0；打开或任一条目读取失败返回 1。
 */
static int
directory_total(char *path, struct ls_options *options, uint64 *total)
{
  char full_path[LS_PATH_SIZE];
  char name[DIRSIZ + 1];
  int fd;
  int failed;
  int status;
  int count;
  struct dirent entry;
  struct stat st;

  *total = 0;
  fd = open(path, O_RDONLY);
  if(fd < 0){
    fprintf(2, "ls: cannot reopen '%s' for total\n", path);
    return 1;
  }

  failed = 0;
  while((count = read(fd, &entry, sizeof(entry))) == sizeof(entry)){
    status = load_directory_entry(path, &entry, options, name, full_path, &st, 0);
    if(status > 0)
      *total += st.size;
    else if(status < 0)
      failed = 1;
  }
  if(count != 0)
    failed = 1;
  close(fd);
  return failed;
}

/**
 * 遍历并输出一个真实目录，不跟随目录中的符号链接。
 *
 * @param path 目录路径。
 * @param options 当前展示选项。
 * @return 全部条目成功时返回 0；任一条目路径过长、stat 或读取失败时返回 1。
 */
static int
list_directory(char *path, struct ls_options *options)
{
  char full_path[LS_PATH_SIZE];
  char name[DIRSIZ + 1];
  char total_text[LS_SIZE_SIZE];
  int fd;
  int failed;
  int printed;
  int status;
  int count;
  uint64 total;
  struct dirent entry;
  struct stat st;

  fd = open(path, O_RDONLY);
  if(fd < 0){
    fprintf(2, "ls: cannot open '%s'\n", path);
    return 1;
  }

  failed = 0;
  if(options->long_format){
    if(directory_total(path, options, &total) != 0)
      failed = 1;
    format_size(total, options->human_readable, total_text);
    printf("total %s\n", total_text);
  }

  printed = 0;
  while((count = read(fd, &entry, sizeof(entry))) == sizeof(entry)){
    status = load_directory_entry(path, &entry, options, name, full_path, &st, 1);
    if(status == 0)
      continue;
    if(status < 0){
      failed = 1;
      continue;
    }
    if(options->long_format){
      print_long_entry(name, &st, options);
    } else {
      if(printed)
        printf(" ");
      print_styled_name(name, st.type);
      printed = 1;
    }
  }
  if(count != 0)
    failed = 1;
  if(!options->long_format)
    printf("\n");
  close(fd);
  return failed;
}

/**
 * 输出一个文件或目录参数。
 *
 * @param path 用户提供的路径。
 * @param options 当前展示选项。
 * @return 成功返回 0；路径不可访问或目录遍历出现错误时返回 1。
 */
static int
list_path(char *path, struct ls_options *options)
{
  char name[DIRSIZ + 2];
  struct stat st;

  if(stat_no_follow(path, &st) < 0){
    fprintf(2, "ls: cannot access '%s'\n", path);
    return 1;
  }
  if(st.type == T_DIR)
    return list_directory(path, options);

  if(options->long_format)
    print_long_entry(display_name(path, name), &st, options);
  else {
    print_styled_name(display_name(path, name), st.type);
    printf("\n");
  }
  return 0;
}

/**
 * 解析命令行并依次处理全部路径。
 *
 * @param argc 命令行参数数量。
 * @param argv 命令行参数数组。
 * @return 通过 exit status 返回：成功为 0，路径错误为 1，参数错误为 2。
 */
int
main(int argc, char *argv[])
{
  struct ls_options options;
  int first_path;
  int failed;
  int path_count;
  int i;

  if(parse_options(argc, argv, &options, &first_path) < 0)
    exit(2);
  if(options.show_help){
    usage();
    exit(0);
  }
  if(first_path == argc)
    exit(list_path(".", &options));

  failed = 0;
  path_count = argc - first_path;
  for(i = first_path; i < argc; i++){
    if(path_count > 1){
      if(i > first_path)
        printf("\n");
      printf("%s:\n", argv[i]);
    }
    if(list_path(argv[i], &options) != 0)
      failed = 1;
  }
  exit(failed);
}
