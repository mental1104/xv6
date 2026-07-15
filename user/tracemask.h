#ifndef XV6_USER_TRACEMASK_H
#define XV6_USER_TRACEMASK_H

// Describes whether a trace specification was parsed successfully and, when it
// failed, which class of input error the caller should report to the user.
enum trace_mask_status {
  TRACE_MASK_OK = 0,       // Parsing succeeded and the output mask is valid.
  TRACE_MASK_EMPTY = -1,   // The specification or output pointer is missing.
  TRACE_MASK_FORMAT = -2,  // A comma-separated list contains an empty field.
  TRACE_MASK_UNKNOWN = -3, // A field is not a registered system call name.
  TRACE_MASK_RANGE = -4,   // A decimal mask exceeds the signed int range.
};

// Convert a user-facing trace specification into the integer mask expected by
// the existing trace system call.
//
// spec: A decimal mask such as "32", a system call name such as "read", or a
//       comma-separated name list such as "read,write".
// mask: Receives the parsed mask only when TRACE_MASK_OK is returned.
//
// Returns one of enum trace_mask_status. The function does not modify *mask on
// failure, allowing callers and tests to detect accidental partial results.
int trace_parse_mask(const char *spec, int *mask);

#endif
