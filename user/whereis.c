#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"

/**
 * 检查候选路径是否指向普通文件，并在命中时输出该路径。
 *
 * @param candidate 待检查的 NUL 结尾路径。
 * @return 候选存在且 inode 类型为 T_FILE 时返回 1，否则返回 0。
 *
 * xv6 尚未实现 Unix 可执行权限位，因此 whereis 与当前 exec 模型一致，只把
 * PATH 目录中的普通文件视为可执行程序候选。
 */
static int
print_file_candidate(char *candidate)
{
  struct stat st;

  if(stat(candidate, &st) < 0 || st.type != T_FILE)
    return 0;
  printf(" %s", candidate);
  return 1;
}

/**
 * 按现有 execvpe() 规则从左到右扫描 PATH，并输出一个名称的全部匹配项。
 *
 * @param name 命令名或包含斜杠的显式路径。
 * @param path PATH 环境变量值；0 表示没有可搜索目录。
 * @return 输出的匹配路径数量。
 *
 * 包含斜杠的名称只检查自身。空 PATH 分量表示当前目录；单个过长候选会被跳过，
 * 但后续目录仍继续参与搜索。
 */
static int
print_matches(char *name, char *path)
{
  char candidate[MAXPATH];
  char *cursor;
  char *end;
  uint directory_length;
  uint name_length;
  uint separator_length;
  int matches = 0;

  if(strchr(name, '/') != 0)
    return print_file_candidate(name);
  if(path == 0)
    return 0;

  name_length = strlen(name);
  cursor = path;
  for(;;){
    end = cursor;
    while(*end != 0 && *end != ':')
      end++;
    directory_length = end - cursor;
    separator_length = directory_length > 0 && cursor[directory_length - 1] != '/';

    if(directory_length + separator_length + name_length + 1 <= sizeof(candidate)){
      if(directory_length > 0)
        memmove(candidate, cursor, directory_length);
      if(separator_length)
        candidate[directory_length] = '/';
      memmove(candidate + directory_length + separator_length,
              name, name_length + 1);
      matches += print_file_candidate(candidate);
    }

    if(*end == 0)
      break;
    cursor = end + 1;
  }
  return matches;
}

/**
 * 查询一个或多个命令在当前环境 PATH 中的位置。
 *
 * @param argc 参数数量；至少包含程序名和一个待查询名称。
 * @param argv 参数数组，argv[1..] 为命令名或显式路径。
 * @param envp 由 execve() 传入的环境向量，用于读取 PATH。
 * @return 不直接返回；全部名称找到时 exit(0)，任一未找到时 exit(1)，参数错误时
 *         exit(2)。
 */
int
main(int argc, char **argv, char **envp)
{
  char *path;
  int missing = 0;
  int i;

  if(argc < 2){
    fprintf(2, "Usage: whereis name...\n");
    exit(2);
  }

  path = envget(envp, "PATH");
  for(i = 1; i < argc; i++){
    printf("%s:", argv[i]);
    if(print_matches(argv[i], path) == 0)
      missing = 1;
    printf("\n");
  }
  exit(missing);
}
