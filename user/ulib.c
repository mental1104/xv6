#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "user/user.h"

char*
strcpy(char *s, const char *t)
{
  char *os;

  os = s;
  while((*s++ = *t++) != 0)
    ;
  return os;
}

int
strcmp(const char *p, const char *q)
{
  while(*p && *p == *q)
    p++, q++;
  return (uchar)*p - (uchar)*q;
}

uint
strlen(const char *s)
{
  int n;

  for(n = 0; s[n]; n++)
    ;
  return n;
}

void*
memset(void *dst, int c, uint n)
{
  char *cdst = (char *) dst;
  int i;
  for(i = 0; i < n; i++){
    cdst[i] = c;
  }
  return dst;
}

char*
strchr(const char *s, char c)
{
  for(; *s; s++)
    if(*s == c)
      return (char*)s;
  return 0;
}

char*
gets(char *buf, int max)
{
  int i, cc;
  char c;

  for(i=0; i+1 < max; ){
    cc = read(0, &c, 1);
    if(cc < 1)
      break;
    buf[i++] = c;
    if(c == '\n' || c == '\r')
      break;
  }
  buf[i] = '\0';
  return buf;
}

int
stat(const char *n, struct stat *st)
{
  int fd;
  int r;

  fd = open(n, O_RDONLY);
  if(fd < 0)
    return -1;
  r = fstat(fd, st);
  close(fd);
  return r;
}

int
atoi(const char *s)
{
  int n;

  n = 0;
  while('0' <= *s && *s <= '9')
    n = n*10 + *s++ - '0';
  return n;
}

void*
memmove(void *vdst, const void *vsrc, int n)
{
  char *dst;
  const char *src;

  dst = vdst;
  src = vsrc;
  if (src > dst) {
    while(n-- > 0)
      *dst++ = *src++;
  } else {
    dst += n;
    src += n;
    while(n-- > 0)
      *--dst = *--src;
  }
  return vdst;
}

int
memcmp(const void *s1, const void *s2, uint n)
{
  const char *p1 = s1, *p2 = s2;
  while (n-- > 0) {
    if (*p1 != *p2) {
      return *p1 - *p2;
    }
    p1++;
    p2++;
  }
  return 0;
}

void *
memcpy(void *dst, const void *src, uint n)
{
  return memmove(dst, src, n);
}

/** 判断一个 NAME=VALUE 项是否与给定变量名精确匹配。 */
static int
environment_name_matches(char *entry, const char *name)
{
  if(entry == 0 || name == 0 || name[0] == 0)
    return 0;
  while(*name != 0 && *entry == *name){
    entry++;
    name++;
  }
  return *name == 0 && *entry == '=';
}

/**
 * 在显式环境向量中查找 NAME=VALUE 项。
 *
 * @param envp 以空指针结尾的环境数组；0 等价于空环境。
 * @param name 不含等号的变量名。
 * @return 命中时返回 VALUE 首字符地址，否则返回 0。
 */
char *
envget(char **envp, const char *name)
{
  char **entry;

  if(envp == 0)
    return 0;
  for(entry = envp; *entry != 0; entry++){
    if(environment_name_matches(*entry, name))
      return strchr(*entry, '=') + 1;
  }
  return 0;
}

/**
 * 使用 PATH 从左到右构造候选路径并调用 execve()。
 *
 * @param program 命令名或包含斜杠的显式路径。
 * @param argv 传给目标程序的参数向量。
 * @param envp 同时提供 PATH 和目标程序环境。
 * @return 所有候选均执行失败时返回 -1；成功后由新程序替换当前进程。
 *
 * 空 PATH 分量表示当前目录。候选路径超过 MAXPATH 时只跳过该分量，避免一个无效
 * 目录阻止后续合法目录继续参与搜索。
 */
int
execvpe(char *program, char **argv, char **envp)
{
  char candidate[MAXPATH];
  char *path;
  char *cursor;
  char *end;
  uint directory_length;
  uint program_length;
  uint separator_length;

  if(program == 0)
    return -1;
  if(strchr(program, '/') != 0)
    return execve(program, argv, envp);

  path = envget(envp, "PATH");
  if(path == 0)
    return -1;
  program_length = strlen(program);
  cursor = path;

  for(;;){
    end = cursor;
    while(*end != 0 && *end != ':')
      end++;
    directory_length = end - cursor;
    separator_length = directory_length > 0 && cursor[directory_length - 1] != '/';

    if(directory_length + separator_length + program_length + 1 <= sizeof(candidate)){
      if(directory_length > 0)
        memmove(candidate, cursor, directory_length);
      if(separator_length)
        candidate[directory_length] = '/';
      memmove(candidate + directory_length + separator_length,
              program, program_length + 1);
      execve(candidate, argv, envp);
    }

    if(*end == 0)
      break;
    cursor = end + 1;
  }
  return -1;
}
