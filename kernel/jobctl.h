#ifndef XV6_JOBCTL_H
#define XV6_JOBCTL_H

/** 停止目标进程组；已停止成员保持不变。 */
#define JOBCTL_STOP 1
/** 继续目标进程组；STOPPED 成员重新进入 RUNNABLE。 */
#define JOBCTL_CONT 2
/** 终止目标进程组；睡眠或停止成员会被唤醒以完成退出。 */
#define JOBCTL_TERM 3

#endif
