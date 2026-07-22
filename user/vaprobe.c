#include "kernel/types.h"
#include "user/user.h"
#include "user/vaaccesslib.h"

/**
 * vaprobe 在同一进程中交互执行 snapshot、reserve、read、write 和 COW 实验。
 *
 * @param argc 命令行参数数量。
 * @param argv 参数数组，首版不接受额外参数。
 * @return 通过 exit() 返回共享入口的状态码。
 */
int
main(int argc, char **argv)
{
  exit(vaaccess_probe_main(argc, argv));
}
