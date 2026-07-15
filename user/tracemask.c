#include "kernel/syscall.h"
#include "kernel/syscall_names.h"
#include "user/tracemask.h"

#define TRACE_MASK_INT_MAX 2147483647

static int
is_decimal(const char *spec)
{
  int i;

  if(spec[0] == 0)
    return 0;

  for(i = 0; spec[i] != 0; i++){
    if(spec[i] < '0' || spec[i] > '9')
      return 0;
  }
  return 1;
}

static int
parse_decimal(const char *spec, int *mask)
{
  int i;
  int value = 0;

  for(i = 0; spec[i] != 0; i++){
    int digit = spec[i] - '0';
    if(value > (TRACE_MASK_INT_MAX - digit) / 10)
      return TRACE_MASK_RANGE;
    value = value * 10 + digit;
  }

  *mask = value;
  return TRACE_MASK_OK;
}

static int
name_matches(const char *name, const char *start, int length)
{
  int i;

  for(i = 0; i < length; i++){
    if(name[i] == 0 || name[i] != start[i])
      return 0;
  }
  return name[length] == 0;
}

static int
lookup_syscall(const char *start, int length)
{
  int i;

  for(i = 1; i < (int)SYSCALL_NAME_COUNT; i++){
    if(syscall_names[i] && name_matches(syscall_names[i], start, length))
      return i;
  }
  return -1;
}

int
trace_parse_mask(const char *spec, int *mask)
{
  const char *start;
  const char *cursor;
  unsigned int result = 0;

  if(spec == 0 || mask == 0 || spec[0] == 0)
    return TRACE_MASK_EMPTY;

  if(is_decimal(spec))
    return parse_decimal(spec, mask);

  start = spec;
  for(cursor = spec; ; cursor++){
    if(*cursor == ',' || *cursor == 0){
      int length = cursor - start;
      int syscall_number;

      if(length == 0)
        return TRACE_MASK_FORMAT;

      syscall_number = lookup_syscall(start, length);
      if(syscall_number < 0)
        return TRACE_MASK_UNKNOWN;

      result |= 1U << syscall_number;
      if(*cursor == 0)
        break;
      start = cursor + 1;
    }
  }

  *mask = result;
  return TRACE_MASK_OK;
}
