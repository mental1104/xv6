#include "kernel/types.h"
#include "kernel/syscall.h"
#include "kernel/syscall_names.h"
#include "user/user.h"
#include "user/tracemask.h"

// Defines one table-driven parser expectation.
struct trace_mask_case {
  char *spec;         // Input specification passed to trace_parse_mask().
  int expected_status; // Expected enum trace_mask_status result.
  int expected_mask;   // Expected mask when parsing succeeds.
};

// Exercise representative success and failure inputs, then verify that every
// registered system call name maps back to its SYS_* bit.
int
main(void)
{
  // Fixed cases cover list composition, backward compatibility, malformed
  // separators, unknown names, and signed-int overflow.
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
  int failed = 0; // Number of expectations that did not match parser output.
  int case_index; // Index of the fixed parser case currently under test.
  int syscall_number; // SYS_* number checked against the shared name table.

  for(case_index = 0;
      case_index < (int)(sizeof(cases) / sizeof(cases[0]));
      case_index++){
    int mask = -1; // Sentinel output used to detect writes on failed parsing.
    int status;    // Actual parser status for the current fixed case.

    status = trace_parse_mask(cases[case_index].spec, &mask);
    if(status != cases[case_index].expected_status ||
       (status == TRACE_MASK_OK && mask != cases[case_index].expected_mask)){
      printf("tracemasktest: %s: status %d mask %d\n",
             cases[case_index].spec, status, mask);
      failed++;
    }
  }

  // This loop prevents a newly registered system call from being printable by
  // the kernel but unavailable through the user-facing name parser.
  for(syscall_number = 1;
      syscall_number < (int)SYSCALL_NAME_COUNT;
      syscall_number++){
    int mask = 0; // Mask generated from the current registered system call name.
    int status;   // Parser result for the current registered name.

    status = trace_parse_mask(syscall_names[syscall_number], &mask);
    if(status != TRACE_MASK_OK || mask != (int)(1U << syscall_number)){
      printf("tracemasktest: syscall %s: status %d mask %d\n",
             syscall_names[syscall_number], status, mask);
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
