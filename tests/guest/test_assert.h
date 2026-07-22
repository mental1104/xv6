#ifndef XV6_TEST_ASSERT_H
#define XV6_TEST_ASSERT_H

#include "kernel/types.h"
#include "user/user.h"

// 每个 guest 测试程序拥有自己的断言计数；该头文件不引入全局测试注册器。
static int xv6_expect_checks;
static int xv6_expect_errors;

/**
 * 记录整数或指针比较结果。
 *
 * 宏在调用本函数前先保存两侧值，确保带副作用的表达式只求值一次。
 */
static inline void
xv6_expect_value(int matched, char *kind, char *expected_expr,
                  char *actual_expr, uint64 expected, uint64 actual,
                  char *file, int line)
{
  xv6_expect_checks++;
  if(matched)
    return;

  xv6_expect_errors++;
  printf("EXPECT mismatch kind=%s file=%s line=%d expected=%s actual=%s expected_value=%p actual_value=%p\n",
         kind, file, line, expected_expr, actual_expr, expected, actual);
}

/**
 * 记录字符串比较结果；空指针不会传给 strcmp() 或 printf("%s")。
 */
static inline void
xv6_expect_string(int matched, char *expected_expr, char *actual_expr,
                   char *expected, char *actual, char *file, int line)
{
  xv6_expect_checks++;
  if(matched)
    return;

  xv6_expect_errors++;
  printf("EXPECT mismatch kind=STREQ file=%s line=%d expected=%s actual=%s expected_text=%s actual_text=%s\n",
         file, line, expected_expr, actual_expr,
         expected == 0 ? "(null)" : expected,
         actual == 0 ? "(null)" : actual);
}

#define XV6_EXPECT_BINARY(kind, op, expected, actual) do {                 \
  long _xv6_expected = (long)(expected);                                   \
  long _xv6_actual = (long)(actual);                                       \
  xv6_expect_value(_xv6_expected op _xv6_actual, kind, #expected, #actual, \
                    (uint64)_xv6_expected, (uint64)_xv6_actual,             \
                    __FILE__, __LINE__);                                    \
} while(0)

#define EXPECT_EQ(expected, actual) XV6_EXPECT_BINARY("EQ", ==, expected, actual)
#define EXPECT_NE(expected, actual) XV6_EXPECT_BINARY("NE", !=, expected, actual)
#define EXPECT_LT(expected, actual) XV6_EXPECT_BINARY("LT", <, expected, actual)
#define EXPECT_LE(expected, actual) XV6_EXPECT_BINARY("LE", <=, expected, actual)
#define EXPECT_GT(expected, actual) XV6_EXPECT_BINARY("GT", >, expected, actual)
#define EXPECT_GE(expected, actual) XV6_EXPECT_BINARY("GE", >=, expected, actual)

#define EXPECT_TRUE(condition) do {                                        \
  int _xv6_actual = !!(condition);                                         \
  xv6_expect_value(_xv6_actual == 1, "TRUE", "true", #condition, 1,      \
                    (uint64)_xv6_actual, __FILE__, __LINE__);               \
} while(0)

#define EXPECT_FALSE(condition) do {                                       \
  int _xv6_actual = !!(condition);                                         \
  xv6_expect_value(_xv6_actual == 0, "FALSE", "false", #condition, 0,    \
                    (uint64)_xv6_actual, __FILE__, __LINE__);               \
} while(0)

#define EXPECT_NULL(value) do {                                            \
  uint64 _xv6_actual = (uint64)(value);                                    \
  xv6_expect_value(_xv6_actual == 0, "NULL", "NULL", #value, 0,          \
                    _xv6_actual, __FILE__, __LINE__);                       \
} while(0)

#define EXPECT_NOT_NULL(value) do {                                        \
  uint64 _xv6_actual = (uint64)(value);                                    \
  xv6_expect_value(_xv6_actual != 0, "NOT_NULL", "non-NULL", #value, 1,  \
                    _xv6_actual, __FILE__, __LINE__);                       \
} while(0)

#define EXPECT_STREQ(expected, actual) do {                                \
  char *_xv6_expected = (expected);                                        \
  char *_xv6_actual = (actual);                                            \
  int _xv6_matched = (_xv6_expected == 0 && _xv6_actual == 0) ||           \
                     (_xv6_expected != 0 && _xv6_actual != 0 &&             \
                      strcmp(_xv6_expected, _xv6_actual) == 0);             \
  xv6_expect_string(_xv6_matched, #expected, #actual,                       \
                     _xv6_expected, _xv6_actual, __FILE__, __LINE__);       \
} while(0)

#define TEST_CHECKS() (xv6_expect_checks)
#define TEST_ERRORS() (xv6_expect_errors)
#define TEST_STATUS() (xv6_expect_errors == 0 ? 0 : 1)

#define TEST_SUMMARY() do {                                                \
  printf("EXPECT summary checks=%d errors=%d\n",                          \
         xv6_expect_checks, xv6_expect_errors);                             \
} while(0)

#define TEST_EXIT() do {                                                   \
  int _xv6_status = TEST_STATUS();                                         \
  TEST_SUMMARY();                                                          \
  exit(_xv6_status);                                                       \
} while(0)

#define ASSERT_EQ(expected, actual) do {                                   \
  int _xv6_errors_before = TEST_ERRORS();                                  \
  EXPECT_EQ(expected, actual);                                             \
  if(TEST_ERRORS() != _xv6_errors_before)                                  \
    TEST_EXIT();                                                           \
} while(0)

#define ASSERT_TRUE(condition) do {                                        \
  int _xv6_errors_before = TEST_ERRORS();                                  \
  EXPECT_TRUE(condition);                                                  \
  if(TEST_ERRORS() != _xv6_errors_before)                                  \
    TEST_EXIT();                                                           \
} while(0)

#endif
