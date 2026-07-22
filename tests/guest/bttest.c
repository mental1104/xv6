#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "tests/guest/test_assert.h"

/**
 * 显式请求内核打印当前系统调用路径的栈回溯。
 *
 * 返回地址由内核写入控制台，宿主机 runner 负责检查至少三行完整地址；本测试
 * 只验证调试系统调用能够正常返回，避免把普通 sleep() 当成隐藏触发器。
 */
int
main(void)
{
  EXPECT_EQ(0, backtrace());
  TEST_EXIT();
}
