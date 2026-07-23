#include "kernel/types.h"
#include "user/user.h"

/**
 * 验证外部用户程序启动后 console 已恢复 cooked 行规约。
 *
 * 程序先输出 ready，宿主机再输入 `ab<Backspace>c<Enter>`；内核应完成退格编辑
 * 后一次返回 `ac\n`。握手避免测试输入仍停留在父 Shell 的 raw 缓冲区。
 *
 * @return 通过 exit status 返回；成功为 0，读取错误或行规约不符为 1。
 */
int
main(void)
{
  char buffer[8] = {0};
  int count;

  printf("consolelinetest: ready\n");
  count = read(0, buffer, sizeof(buffer));
  if(count != 3 || buffer[0] != 'a' || buffer[1] != 'c' || buffer[2] != '\n'){
    printf("consolelinetest: count=%d bytes=%d,%d,%d\n",
           count, buffer[0], buffer[1], buffer[2]);
    exit(1);
  }
  printf("consolelinetest: OK\n");
  exit(0);
}
