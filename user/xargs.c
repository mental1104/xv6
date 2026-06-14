#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/param.h"

void
run(char *line, int argc, char *argv[])
{
  char *full_argv[MAXARG];
  char *p;
  int i;

  for(i = 1; i < argc; i++)
    full_argv[i-1] = argv[i];

  i = argc - 1;
  p = line;
  while(*p){
    while(*p == ' ' || *p == '\t')
      p++;
    if(*p == 0)
      break;
    if(i >= MAXARG - 1){
      fprintf(2, "xargs: too many args\n");
      exit(1);
    }
    full_argv[i++] = p;
    while(*p && *p != ' ' && *p != '\t')
      p++;
    if(*p)
      *p++ = 0;
  }
  full_argv[i] = 0;

  if(fork() == 0){
    exec(full_argv[0], full_argv);
    fprintf(2, "xargs: exec %s failed\n", full_argv[0]);
    exit(1);
  }

  wait(0);
}

int
main(int argc, char *argv[])
{
  char buf[512];
  char c;
  int n;
  int len;

  if(argc < 2){
    fprintf(2, "usage: xargs command\n");
    exit(1);
  }

  len = 0;
  while((n = read(0, &c, 1)) > 0){
    if(c == '\n'){
      buf[len] = 0;
      if(len > 0)
        run(buf, argc, argv);
      len = 0;
      continue;
    }
    if(len >= sizeof(buf) - 1){
      fprintf(2, "xargs: line too long\n");
      exit(1);
    }
    buf[len++] = c;
  }

  if(len > 0){
    buf[len] = 0;
    run(buf, argc, argv);
  }

  exit(0);
}
