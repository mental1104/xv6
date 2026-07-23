struct stat;
struct rtcdate;
struct sysinfo;
struct memviz_snapshot;
struct memviz_va_query;
struct sched_stats;
struct schedtrace_snapshot;
struct user_context;

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
int open(const char*, int);
int mknod(const char*, short, short);
int unlink(const char*);
int fstat(int fd, struct stat*);
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
int vaquery(uint64 va, struct memviz_va_query *query);
int consolemode(int fd, int mode);
int sched_set_hint(int ticks);
int sched_set_weight(int weight);
int sched_get_stats(struct sched_stats *stats);
int schedtrace(int op, struct schedtrace_snapshot *snapshot, int arg);

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
