#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/tracemask.h"

static void
usage(char *program)
{
  fprintf(2, "Usage: %s mask|syscall[,syscall...] command [args...]\n", program);
}

int
main(int argc, char *argv[])
{
  int i;
  int mask;
  int status;
  char *nargv[MAXARG];

  if(argc < 3){
    usage(argv[0]);
    exit(1);
  }

  status = trace_parse_mask(argv[1], &mask);
  if(status != TRACE_MASK_OK){
    if(status == TRACE_MASK_UNKNOWN)
      fprintf(2, "%s: unknown system call in '%s'\n", argv[0], argv[1]);
    else if(status == TRACE_MASK_RANGE)
      fprintf(2, "%s: integer mask out of range: %s\n", argv[0], argv[1]);
    else
      fprintf(2, "%s: invalid trace specification: %s\n", argv[0], argv[1]);
    usage(argv[0]);
    exit(1);
  }

  if(argc - 2 >= MAXARG){
    fprintf(2, "%s: too many command arguments\n", argv[0]);
    exit(1);
  }

  if(trace(mask) < 0){
    fprintf(2, "%s: trace failed\n", argv[0]);
    exit(1);
  }

  for(i = 2; i < argc; i++)
    nargv[i-2] = argv[i];
  nargv[argc-2] = 0;

  exec(nargv[0], nargv);
  fprintf(2, "%s: exec %s failed\n", argv[0], nargv[0]);
  exit(1);
}
