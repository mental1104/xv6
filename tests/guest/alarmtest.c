//
// test program for the alarm lab.
// you can modify this file for testing,
// but please make sure your kernel
// modifications pass the original
// versions of these tests.
//

#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "user/user.h"

void test0();
void test1();
void test2();
void periodic();
void slow_handler();

volatile static int count;
volatile static int failed;

/**
 * 运行 alarm Lab 的三个行为测试，并通过退出状态传播汇总结果。
 *
 * @param argc 命令行参数数量，本程序不读取额外参数。
 * @param argv 命令行参数数组，本程序不读取额外参数。
 * @return 本函数通过 exit() 返回：全部通过为 0，任一失败为 1。
 */
int
main(int argc, char *argv[])
{
  failed = 0;
  test0();
  test1();
  test2();
  exit(failed != 0);
}

/**
 * 周期性 alarm handler，记录调用次数并恢复被中断的用户上下文。
 *
 * @return 不返回普通调用者；通过 sigreturn() 恢复 trap 前上下文。
 */
void
periodic()
{
  count = count + 1;
  printf("alarm!\n");
  sigreturn();
}

// tests whether the kernel calls
// the alarm handler even a single time.
void
test0()
{
  int i;
  printf("test0 start\n");
  count = 0;
  sigalarm(2, periodic);
  for(i = 0; i < 1000*500000; i++){
    if((i % 1000000) == 0)
      write(2, ".", 1);
    if(count > 0)
      break;
  }
  sigalarm(0, 0);
  if(count > 0){
    printf("test0 passed\n");
  } else {
    printf("\ntest0 failed: the kernel never called the alarm handler\n");
    failed = 1;
  }
}

void __attribute__ ((noinline)) foo(int i, int *j) {
  if((i % 2500000) == 0) {
    write(2, ".", 1);
  }
  *j += 1;
}

//
// tests that the kernel calls the handler multiple times.
//
// tests that, when the handler returns, it returns to
// the point in the program where the timer interrupt
// occurred, with all registers holding the same values they
// held when the interrupt occurred.
//
void
test1()
{
  int i;
  int j;

  printf("test1 start\n");
  count = 0;
  j = 0;
  sigalarm(2, periodic);
  for(i = 0; i < 500000000; i++){
    if(count >= 10)
      break;
    foo(i, &j);
  }
  sigalarm(0, 0);
  if(count < 10){
    printf("\ntest1 failed: too few calls to the handler\n");
    failed = 1;
  } else if(i != j){
    // the loop should have called foo() i times, and foo() should
    // have incremented j once per call, so j should equal i.
    // once possible source of errors is that the handler may
    // return somewhere other than where the timer interrupt
    // occurred; another is that that registers may not be
    // restored correctly, causing i or j or the address ofj
    // to get an incorrect value.
    printf("\ntest1 failed: foo() executed fewer times than it was called\n");
    failed = 1;
  } else {
    printf("test1 passed\n");
  }
}

//
// tests that kernel does not allow reentrant alarm calls.
void
test2()
{
  int i;
  int pid;
  int status;
  int waited_pid;

  printf("test2 start\n");
  pid = fork();
  if(pid < 0){
    printf("test2 failed: fork failed\n");
    failed = 1;
    return;
  }
  if(pid == 0){
    count = 0;
    sigalarm(2, slow_handler);
    for(i = 0; i < 1000*500000; i++){
      if((i % 1000000) == 0)
        write(2, ".", 1);
      if(count > 0)
        break;
    }
    if(count == 0){
      printf("\ntest2 failed: alarm not called\n");
      exit(1);
    }
    exit(0);
  }

  waited_pid = wait(&status);
  if(waited_pid != pid || status != 0){
    printf("test2 failed: child status=%d waited=%d expected=%d\n",
           status, waited_pid, pid);
    failed = 1;
    return;
  }
  printf("test2 passed\n");
}

/**
 * 长时间运行的 alarm handler，用于验证内核不会重入当前 handler。
 *
 * @return 不返回普通调用者；成功路径关闭 alarm 并通过 sigreturn() 恢复。
 */
void
slow_handler()
{
  count++;
  printf("alarm!\n");
  if(count > 1){
    printf("test2 failed: alarm handler called more than once\n");
    exit(1);
  }
  for(int i = 0; i < 1000*500000; i++){
    asm volatile("nop"); // avoid compiler optimizing away loop
  }
  sigalarm(0, 0);
  sigreturn();
}
