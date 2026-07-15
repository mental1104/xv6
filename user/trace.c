#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/tracemask.h"

// Print the accepted trace command syntax.
//
// program: Executable name shown at the start of the usage message.
static void
usage(char *program)
{
  fprintf(2, "Usage: %s mask|syscall[,syscall...] command [args...]\n", program);
}

// Parse the requested trace mask, enable tracing for the current process, and
// replace this wrapper with the target command through exec().
//
// argc: Number of command-line arguments, including the trace program name.
// argv: Argument vector whose second entry is the trace specification and whose
//       remaining entries form the target command and its arguments.
int
main(int argc, char *argv[])
{
  int argument_index; // Index used while copying the target command arguments.
  int mask;           // Integer bit mask passed to the existing trace syscall.
  int parse_status;   // Detailed result returned by trace_parse_mask().
  char *command_argv[MAXARG]; // NUL-terminated argv passed to the target command.

  if(argc < 3){
    usage(argv[0]);
    exit(1);
  }

  parse_status = trace_parse_mask(argv[1], &mask);
  if(parse_status != TRACE_MASK_OK){
    if(parse_status == TRACE_MASK_UNKNOWN)
      fprintf(2, "%s: unknown system call in '%s'\n", argv[0], argv[1]);
    else if(parse_status == TRACE_MASK_RANGE)
      fprintf(2, "%s: integer mask out of range: %s\n", argv[0], argv[1]);
    else
      fprintf(2, "%s: invalid trace specification: %s\n", argv[0], argv[1]);
    usage(argv[0]);
    exit(1);
  }

  // Reserve one final slot for the NUL pointer required by exec().
  if(argc - 2 >= MAXARG){
    fprintf(2, "%s: too many command arguments\n", argv[0]);
    exit(1);
  }

  // trace() changes only the current process. exec() then preserves that
  // process-level mask while replacing the wrapper program with the command.
  if(trace(mask) < 0){
    fprintf(2, "%s: trace failed\n", argv[0]);
    exit(1);
  }

  for(argument_index = 2; argument_index < argc; argument_index++)
    command_argv[argument_index - 2] = argv[argument_index];
  command_argv[argc - 2] = 0;

  exec(command_argv[0], command_argv);
  fprintf(2, "%s: exec %s failed\n", argv[0], command_argv[0]);
  exit(1);
}
