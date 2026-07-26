#ifndef XV6_KERNEL_LOG_H
#define XV6_KERNEL_LOG_H

// 教学实验使用的一次性日志崩溃注入点。编号同时作为用户态 logcrash() 参数。
#define LOG_CRASH_NONE            0
#define LOG_CRASH_BEFORE_COMMIT   1
#define LOG_CRASH_AFTER_COMMIT    2
#define LOG_CRASH_DURING_INSTALL  3

#endif
