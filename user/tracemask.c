#include "kernel/syscall.h"
#include "kernel/syscall_names.h"
#include "user/tracemask.h"

// Largest decimal mask accepted by trace(int), whose argument is a signed int.
#define TRACE_MASK_INT_MAX 2147483647

// Return non-zero only when spec is a non-empty string containing decimal
// digits and no system call names or separators.
//
// spec: Candidate trace specification to classify.
static int
is_decimal(const char *spec)
{
  int index; // Current character offset within spec.

  if(spec[0] == 0)
    return 0;

  for(index = 0; spec[index] != 0; index++){
    if(spec[index] < '0' || spec[index] > '9')
      return 0;
  }
  return 1;
}

// Parse a decimal trace mask while detecting signed-int overflow before each
// multiplication and addition.
//
// spec: Non-empty decimal string previously validated by is_decimal().
// mask: Receives the parsed integer when TRACE_MASK_OK is returned.
static int
parse_decimal(const char *spec, int *mask)
{
  int index;     // Current digit offset within spec.
  int value = 0; // Decimal value accumulated from the processed prefix.

  for(index = 0; spec[index] != 0; index++){
    int digit = spec[index] - '0'; // Numeric value of the current character.

    // Rearranging value * 10 + digit <= INT_MAX avoids overflowing while
    // checking whether the next digit can be appended safely.
    if(value > (TRACE_MASK_INT_MAX - digit) / 10)
      return TRACE_MASK_RANGE;
    value = value * 10 + digit;
  }

  *mask = value;
  return TRACE_MASK_OK;
}

// Compare one registered, NUL-terminated system call name with a bounded field
// from the comma-separated user specification.
//
// name:   Registered system call name from syscall_names[].
// start:  Address of the first character in the candidate field.
// length: Number of characters in the candidate field, excluding separators.
static int
name_matches(const char *name, const char *start, int length)
{
  int index; // Character offset compared in both strings.

  for(index = 0; index < length; index++){
    if(name[index] == 0 || name[index] != start[index])
      return 0;
  }
  return name[length] == 0;
}

// Resolve one bounded name field to its system call number.
//
// start:  Address of the field inside the original trace specification.
// length: Number of characters belonging to the field.
//
// Returns the positive SYS_* number on success, or -1 for an unknown name.
static int
lookup_syscall(const char *start, int length)
{
  int syscall_number; // Candidate index into the shared syscall_names table.

  for(syscall_number = 1;
      syscall_number < (int)SYSCALL_NAME_COUNT;
      syscall_number++){
    if(syscall_names[syscall_number] &&
       name_matches(syscall_names[syscall_number], start, length))
      return syscall_number;
  }
  return -1;
}

// Convert a decimal mask or comma-separated system call name list into the
// integer bit mask consumed by the existing trace(int) system call.
//
// spec: A decimal mask, one system call name, or comma-separated names.
// mask: Receives the final mask only after the complete input is validated.
int
trace_parse_mask(const char *spec, int *mask)
{
  const char *field_start; // First character of the field being parsed.
  const char *cursor;      // Character currently inspected for ',' or NUL.
  unsigned int result = 0; // OR-combined mask for all validated name fields.

  if(spec == 0 || mask == 0 || spec[0] == 0)
    return TRACE_MASK_EMPTY;

  // Preserve the original Lab2 command interface when every character is a
  // decimal digit; otherwise interpret the specification as system call names.
  if(is_decimal(spec))
    return parse_decimal(spec, mask);

  field_start = spec;
  for(cursor = spec; ; cursor++){
    if(*cursor == ',' || *cursor == 0){
      int field_length = cursor - field_start; // Size of the completed field.
      int syscall_number; // SYS_* number resolved from the completed field.

      if(field_length == 0)
        return TRACE_MASK_FORMAT;

      syscall_number = lookup_syscall(field_start, field_length);
      if(syscall_number < 0)
        return TRACE_MASK_UNKNOWN;

      // Repeated names are intentionally idempotent because setting an already
      // set bit does not change the accumulated mask.
      result |= 1U << syscall_number;
      if(*cursor == 0)
        break;
      field_start = cursor + 1;
    }
  }

  *mask = result;
  return TRACE_MASK_OK;
}
