# Lightweight guest assertions

在 xv6 guest 测试中包含：

```c
#include "tests/guest/test_assert.h"
```

可用宏：

```c
EXPECT_EQ(expected, actual);
EXPECT_NE(expected, actual);
EXPECT_LT(expected, actual);
EXPECT_LE(expected, actual);
EXPECT_GT(expected, actual);
EXPECT_GE(expected, actual);
EXPECT_TRUE(condition);
EXPECT_FALSE(condition);
EXPECT_NULL(pointer);
EXPECT_NOT_NULL(pointer);
EXPECT_STREQ(expected, actual);
ASSERT_EQ(expected, actual);
ASSERT_TRUE(condition);
TEST_EXIT();
```

`EXPECT_*` 记录错误并继续执行，适合一次报告多个不变量；`ASSERT_*` 在当前断言失败后立即打印汇总并退出。测试主函数最后必须调用 `TEST_EXIT()`，由退出状态交给 `xv6test` 汇总。

示例：

```c
int
main(void)
{
  int value = 3;
  char *name = "xv6";

  EXPECT_EQ(3, value);
  EXPECT_TRUE(value > 0);
  EXPECT_STREQ("xv6", name);
  TEST_EXIT();
}
```

范围刻意保持最小：没有 fixture、mock、参数化测试、自动注册、动态分配或宿主机依赖。整数/指针比较按 RV64 的 `long` 保存，两侧表达式只求值一次。
