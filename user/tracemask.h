#ifndef XV6_USER_TRACEMASK_H
#define XV6_USER_TRACEMASK_H

enum trace_mask_status {
  TRACE_MASK_OK = 0,
  TRACE_MASK_EMPTY = -1,
  TRACE_MASK_FORMAT = -2,
  TRACE_MASK_UNKNOWN = -3,
  TRACE_MASK_RANGE = -4,
};

int trace_parse_mask(const char *spec, int *mask);

#endif
