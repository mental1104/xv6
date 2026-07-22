#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "user/user.h"

/**
 * 触发一次可观察的 read trace，并用真实 read 结果验证系统调用正常完成。
 *
 * trace 文本由内核控制台输出，guest 无法通过普通文件描述符捕获；因此本程序
 * 负责状态验证，宿主机 runner 只在所属 Lab2 group 日志中补充检查 trace 行。
 *
 * @return 本函数通过 exit() 返回：open、trace 或 read 失败为 1，成功为 0。
 */
int
main(void)
{
  char buffer[16];
  int fd = open("README", O_RDONLY);

  if(fd < 0){
    printf("tracesmoke: open README failed\n");
    exit(1);
  }
  if(trace(1U << SYS_read) < 0){
    close(fd);
    printf("tracesmoke: trace enable failed\n");
    exit(1);
  }
  if(read(fd, buffer, sizeof(buffer)) <= 0){
    trace(0);
    close(fd);
    printf("tracesmoke: read failed\n");
    exit(1);
  }
  trace(0);
  close(fd);
  printf("tracesmoke: OK\n");
  exit(0);
}
