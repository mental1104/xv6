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
 * memviz_print_filtered 采集并打印一个带用户过滤条件的内存视图。
 *
 * @param view MEMVIZ_VIEW_* 之一；当前只有 pagetable 使用 filter。
 * @param plain 非零时禁用 ANSI 颜色。
 * @param filter 过滤字符串；为空指针时打印默认完整视图。
 * @return 成功返回 0；系统调用、view 或 filter 无效时返回 -1。
 */
int memviz_print_filtered(int view, int plain, char *filter);

/**
 * memviz_print_all 依次打印 user、pagetable、phys 和 kernel 四个视图。
 *
 * @param plain 非零时禁用 ANSI 颜色。
 * @return 全部成功返回 0；任一视图失败时返回 -1。
 */
int memviz_print_all(int plain);

#endif
