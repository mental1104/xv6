#ifndef XV6_USER_POLLREAD_H
#define XV6_USER_POLLREAD_H

/**
 * 返回 pipe 读端的当前就绪位图，或等待其中任一读端就绪。
 *
 * @param fds pipe 读端描述符数组。
 * @param count 数组中的有效描述符数量，范围为 1 到 NOFILE。
 * @param wait 非零时阻塞等待，为零时只读取当前快照。
 * @return 成功返回按数组槽位编码的非负位图；参数非法时返回 -1。
 */
int pollread(int *fds, int count, int wait);

#endif
