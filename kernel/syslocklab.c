#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

/**
 * 将用户态锁实验请求转发给内核中的受控实验状态机。
 *
 * @return locklab_run() 的操作结果；当前接口只有两个整数参数，不复制用户指针。
 */
uint64
sys_locklab(void)
{
  int operation;
  int value;

  argint(0, &operation);
  argint(1, &value);
  return locklab_run(operation, value);
}
