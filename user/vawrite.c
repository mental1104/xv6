#include "kernel/types.h"
#include "user/user.h"
#include "user/vaaccesslib.h"

/**
 * vawrite 从用户态写入一个字节，真实访问由 worker 执行。
 *
 * @param argc 命令行参数数量。
 * @param argv 参数数组，详见 vaaccess_write_main()。
 * @return 通过 exit() 返回共享入口的状态码。
 */
int
main(int argc, char **argv)
{
  exit(vaaccess_write_main(argc, argv));
}
