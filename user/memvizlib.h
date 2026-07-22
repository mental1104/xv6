#ifndef XV6_USER_MEMVIZLIB_H
#define XV6_USER_MEMVIZLIB_H

/**
 * memviz_print 采集并打印一个内存视图。
 *
 * @param view MEMVIZ_VIEW_* 之一。
 * @param plain 非零时禁用 ANSI 颜色，便于日志和自动化测试。
 * @return 成功返回 0；系统调用或 view 无效时返回 -1。
 */
int memviz_print(int view, int plain);

/**
 * memviz_print_all 依次打印 user、phys 和 kernel 三个视图。
 *
 * @param plain 非零时禁用 ANSI 颜色。
 * @return 全部成功返回 0；任一视图失败时返回 -1。
 */
int memviz_print_all(int plain);

#endif
