#include "kernel/types.h"
#include "kernel/syscall.h"
#include "kernel/syscall_names.h"
#include "user/user.h"
#include "user/tracemask.h"

struct trace_mask_case {
  char *spec;
  int expected_status;
  int expected_mask;
};

int
main(void)
{
  struct trace_mask_case cases[] = {
    {"read", TRACE_MASK_OK, 1U << SYS_read},
    {"read,write", TRACE_MASK_OK, (1U << SYS_read) | (1U << SYS_write)},
    {"read,read", TRACE_MASK_OK, 1U << SYS_read},
    {"32", TRACE_MASK_OK, 32},
    {"0", TRACE_MASK_OK, 0},
    {"", TRACE_MASK_EMPTY, 0},
    {"unknown", TRACE_MASK_UNKNOWN, 0},
    {"read,,write", TRACE_MASK_FORMAT, 0},
    {",read", TRACE_MASK_FORMAT, 0},
    {"read,", TRACE_MASK_FORMAT, 0},
    {"2147483648", TRACE_MASK_RANGE, 0},
    {"read|write", TRACE_MASK_UNKNOWN, 0},
  };
  int failed = 0;
  int i;

  for(i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++){
    int mask = -1;
    int status = trace_parse_mask(cases[i].spec, &mask);

    if(status != cases[i].expected_status ||
       (status == TRACE_MASK_OK && mask != cases[i].expected_mask)){
      printf("tracemasktest: %s: status %d mask %d\n",
             cases[i].spec, status, mask);
      failed++;
    }
  }

  for(i = 1; i < (int)SYSCALL_NAME_COUNT; i++){
    int mask = 0;
    int status = trace_parse_mask(syscall_names[i], &mask);

    if(status != TRACE_MASK_OK || mask != (int)(1U << i)){
      printf("tracemasktest: syscall %s: status %d mask %d\n",
             syscall_names[i], status, mask);
      failed++;
    }
  }

  if(failed){
    printf("tracemasktest: %d tests failed\n", failed);
    exit(1);
  }

  printf("tracemasktest: all tests passed\n");
  exit(0);
}
