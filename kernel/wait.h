#ifndef XV6_WAIT_H
#define XV6_WAIT_H

/** waitpid() 在匹配子进程仍运行时立即返回 0。 */
#define WNOHANG 1
/* 值 2 保留为历史非法 options 回归，避免扩展后悄悄改变旧测试语义。 */
/** waitpid() 允许返回尚未退出的 STOPPED 子进程事件。 */
#define WUNTRACED 4
/** waitpid() 允许返回 STOPPED -> RUNNABLE 的继续事件。 */
#define WCONTINUED 8

/*
 * xv6 仍直接返回普通 exit status；两个高位常量只标识作业控制事件，
 * 避免改变既有 wait()/waitpid() 的退出状态兼容性。
 */
#define WAIT_STATUS_STOPPED   0x40000000
#define WAIT_STATUS_CONTINUED 0x20000000

#define WIFSTOPPED(status)   ((status) == WAIT_STATUS_STOPPED)
#define WIFCONTINUED(status) ((status) == WAIT_STATUS_CONTINUED)
#define WIFEXITED(status)    (!WIFSTOPPED(status) && !WIFCONTINUED(status))
#define WEXITSTATUS(status)  (status)

#endif
