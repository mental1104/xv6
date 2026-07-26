#ifndef XV6_USER_USER_H
#define XV6_USER_USER_H

struct stat;
struct rtcdate;
struct sysinfo;
struct procinfo;
struct memviz_snapshot;
struct memviz_va_query;
struct fsinspect_snapshot;
struct sched_stats;
struct schedtrace_snapshot;
struct disktrace_snapshot;
struct user_context;
struct swap_info;

// system calls
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int*);
int waitpid(int, int*, int);
int pipe(int*);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int);
int exec(char*, char**);

/**
 * 用指定程序替换当前用户镜像，并向入口传递参数与环境向量。
 *
 * @param path ELF 程序路径。
 * @param argv 以空指针结尾的参数数组，最多 MAXARG 项。
 * @param envp 以空指针结尾的 NAME=VALUE 环境数组，最多 MAXENV 项；0 表示空环境。
 * @return 仅失败时返回 -1；成功后从新程序的 main(argc, argv, envp) 开始执行。
 */
int execve(char*, char**, char**);

int open(const char*, int);
int mknod(const char*, short, short);
int unlink(const char*);
int fstat(int fd, struct stat*);

/**
 * 重新定位普通文件描述符的共享 64 位偏移。
 *
 * @param fd 已打开的普通文件描述符；pipe 和 device 不支持定位。
 * @param offset 相对 whence 基准的有符号字节偏移。
 * @param whence SEEK_SET、SEEK_CUR 或 SEEK_END。
 * @return 成功返回新的非负偏移；参数非法或结果越界时返回 -1。
 */
int64 lseek(int fd, int64 offset, int whence);

int link(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
char* sbrk(int);
int sleep(int);
int uptime(void);
int trace(uint64);
int sysinfo(struct sysinfo*);
int sigalarm(int ticks, void (*handler)());
int sigreturn(void);
int symlink(char *target, char *path);
char *mmap(void *addr, int length, int prot, int flags, int fd, int offset);
int munmap(void *addr, int length);
int backtrace(void);
int memsnapshot(int view, struct memviz_snapshot *snapshot);

/**
 * 只读采集指定进程的稳定内存快照。
 *
 * @param pid 目标进程 PID，必须为正数。
 * @param view MEMVIZ_VIEW_* 之一。
 * @param snapshot 接收快照的用户缓冲区。
 * @return 当前进程或稳定的非 RUNNING 目标成功返回 0；否则返回 -1。
 */
int memsnapshot_pid(int pid, int view, struct memviz_snapshot *snapshot);

int vaquery(uint64 va, struct memviz_va_query *query);
int consolemode(int fd, int mode);
int sched_set_hint(int ticks);
int sched_set_weight(int weight);
int sched_get_stats(struct sched_stats *stats);
int schedtrace(int op, struct schedtrace_snapshot *snapshot, int arg);

/**
 * 读取文件系统全局状态，并可选附带一个打开 inode 的块映射边界。
 *
 * @param fd 普通文件或设备描述符；FSINSPECT_GLOBAL_FD 只读取全局状态。
 * @param snapshot 接收 kernel/fsinspect.h 定义的结构化观察结果。
 * @return 成功返回 0；描述符、对象类型或用户地址非法时返回 -1。
 */
int fsinspect(int fd, struct fsinspect_snapshot *snapshot);

/**
 * 驱动显式启用的并发入门教学会话。
 *
 * @param op kernel/concurrencylab.h 定义的 RESET、RUN、SNAPSHOT 或 CLOSE。
 * @param arg0 RESET 时为模式，其他操作时为会话编号。
 * @param arg1 RUN 时为唯一参与者角色，其他操作忽略。
 * @param result RUN 或 SNAPSHOT 的用户态输出缓冲区；其他操作可传 0。
 * @return RESET 成功返回正会话编号；其他操作成功返回 0；失败返回 -1。
 */
int concurrencylab(int op, int arg0, int arg1, void *result);

/**
 * 控制和读取 xv6 virtio 驱动边界轨迹。
 *
 * @param op DISKTRACE_OP_*。
 * @param snapshot READ 时接收固定容量快照，其余操作可为空。
 * @param max_events READ 时指定愿意接收的事件数。
 * @return 成功返回 0，参数或用户地址非法时返回 -1。
 */
int disktrace(int op, struct disktrace_snapshot *snapshot, int max_events);

/**
 * Explicitly move one current-process anonymous user page to the teaching
 * swap backing file. This exposes mechanism only; no automatic replacement
 * policy is implied.
 */
int swapout(void *address);

/** Return global swap counters and the non-faulting state of one virtual page. */
int swapinfo(void *address, struct swap_info *info);

/**
 * 将当前进程表复制到用户提供的结构化快照数组。
 *
 * @param entries 接收最多 max_entries 个 struct procinfo；结构定义在 kernel/procinfo.h。
 * @param max_entries 数组容量，必须大于 0；超过 NPROC 时由内核钳制。
 * @return 成功返回复制的非 UNUSED 进程数量；参数或用户地址非法时返回 -1。
 */
int getprocs(struct procinfo *entries, int max_entries);

/**
 * 将当前进程或直接子进程放入指定进程组。
 *
 * @param pid 目标 PID；0 表示当前进程。
 * @param pgid 目标 PGID；0 表示使用目标 PID 创建新进程组。
 * @return 成功返回 0；目标不可见、已退出或参数非法时返回 -1。
 */
int setpgid(int pid, int pgid);

/**
 * 查询当前进程或直接子进程的进程组。
 *
 * @param pid 目标 PID；0 表示当前进程。
 * @return 成功返回正 PGID；目标不可见或已退出时返回 -1。
 */
int getpgid(int pid);

/**
 * 对整个进程组执行 JOBCTL_STOP、JOBCTL_CONT 或 JOBCTL_TERM。
 *
 * @param pgid 目标进程组 ID，必须为正数。
 * @param action kernel/jobctl.h 中定义的动作。
 * @return 至少命中一个活跃成员时返回 0，否则返回 -1。
 */
int procctl(int pgid, int action);

/**
 * 将单控制台的前台所有权交给指定进程组。
 *
 * @param pgid 目标进程组 ID；仅交互式 sh 可以调用。
 * @return 成功返回 0；调用者或状态不满足约束时返回 -1。
 */
int tcsetpgrp(int pgid);

/**
 * 保存当前用户现场，并可在同一次系统调用中恢复另一份完整用户现场。
 *
 * @param save 接收当前 epc 和通用寄存器的用户缓冲区；为空时跳过保存。
 * @param restore 要恢复的用户现场；为空时只保存当前现场。
 * @param guard 可选调度临界区标志；内核在恢复目标现场前将其清零。
 * @return 仅保存时返回 0；切换恢复后返回保存现场规定的 a0；地址非法返回 -1。
 */
int ucontext_switch(struct user_context *save,
                    const struct user_context *restore,
                    volatile int *guard);

// ulib.c
int stat(const char*, struct stat*);
char* strcpy(char*, const char*);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
void fprintf(int, const char*, ...);
void printf(const char*, ...);
char* gets(char*, int max);
uint strlen(const char*);
void* memset(void*, int, uint);
void* malloc(uint);
void free(void*);
int atoi(const char*);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);

/**
 * 在显式环境向量中查找变量。
 *
 * @param envp 以空指针结尾的 NAME=VALUE 数组；0 等价于空环境。
 * @param name 不含等号的变量名。
 * @return 找到时返回 VALUE 起始地址，否则返回 0；返回指针由 envp 持有。
 */
char *envget(char **envp, const char *name);

/**
 * 使用 envp 中的 PATH 搜索并执行程序。
 *
 * @param program 命令名或显式路径；包含斜杠时不搜索 PATH。
 * @param argv 传给新程序的参数数组。
 * @param envp 同时用于 PATH 查询和新程序环境继承。
 * @return 仅全部候选执行失败时返回 -1；成功后不会返回。
 */
int execvpe(char *program, char **argv, char **envp);

#endif
