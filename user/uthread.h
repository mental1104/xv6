#ifndef XV6_UTHREAD_H
#define XV6_UTHREAD_H

#define UTHREAD_MAX_WORKERS 16

/** 用户线程入口；argument 的生命周期和所有权由调用者负责。 */
typedef void (*thread_func)(void *argument);

/**
 * 初始化当前进程内的 M:1 用户线程运行时。
 *
 * @return 首次初始化成功返回 0；重复初始化或上下文快照失败返回 -1。
 */
int thread_init(void);

/**
 * 创建一个拥有独立静态栈的用户线程。
 *
 * @param function 线程入口，不能为空；普通返回会自动转换为 thread_exit()。
 * @param argument 原样传给 function 的用户指针，允许为空且不转移所有权。
 * @return 成功时返回 1 到 UTHREAD_MAX_WORKERS 的线程编号；参数非法或容量
 *         耗尽时返回 -1。
 */
int thread_create(thread_func function, void *argument);

/**
 * 启用 timer alarm 驱动的用户态抢占。
 *
 * @return 启动成功或已经启动时返回 0；运行时未初始化或 alarm 注册失败返回 -1。
 */
int thread_start(void);

/** 主动让出当前用户线程；该接口不是其他线程获得执行机会的必要条件。 */
void thread_yield(void);

/**
 * 等待一个工作线程退出并回收其槽位。
 *
 * @param tid thread_create() 返回的工作线程编号，不能是当前线程或主线程 0。
 * @return 等待和回收成功返回 0；编号无效、重复 join 或无法调度时返回 -1。
 */
int thread_join(int tid);

/** 退出当前工作线程；主线程调用时直接结束进程。 */
void thread_exit(void) __attribute__((noreturn));

#endif
