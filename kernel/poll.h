#ifndef XV6_KERNEL_POLL_H
#define XV6_KERNEL_POLL_H

struct file;
struct pipe;

/** 判断 pipe 的下一次读取是否能够立即取得数据或 EOF。 */
int pipereadable(struct pipe *pipe);

/** 初始化教学型 pipe 就绪等待队列。 */
void pollinit(void);

/** 在 pipe 状态变化后唤醒所有就绪等待者。 */
void pollnotify(void);

/** 扫描一组可读 pipe，并按槽位返回就绪位图。 */
int pollreadfiles(struct file **files, int count, int wait);

#endif
