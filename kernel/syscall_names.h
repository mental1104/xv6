#ifndef XV6_SYSCALL_NAMES_H
#define XV6_SYSCALL_NAMES_H

// 将每个 SYS_* 编号映射到命令行解析和内核诊断输出共用的标准名称。
// 使用指定下标初始化，即使系统调用编号的声明顺序发生变化，名称仍能与
// kernel/syscall.h 中的编号保持一致。
static const char *const syscall_names[] = {
[SYS_fork]      = "fork",
[SYS_exit]      = "exit",
[SYS_wait]      = "wait",
[SYS_pipe]      = "pipe",
[SYS_read]      = "read",
[SYS_kill]      = "kill",
[SYS_exec]      = "exec",
[SYS_fstat]     = "fstat",
[SYS_chdir]     = "chdir",
[SYS_dup]       = "dup",
[SYS_getpid]    = "getpid",
[SYS_sbrk]      = "sbrk",
[SYS_sleep]     = "sleep",
[SYS_uptime]    = "uptime",
[SYS_open]      = "open",
[SYS_write]     = "write",
[SYS_mknod]     = "mknod",
[SYS_unlink]    = "unlink",
[SYS_link]      = "link",
[SYS_mkdir]     = "mkdir",
[SYS_close]     = "close",
[SYS_trace]     = "trace",
[SYS_sysinfo]   = "sysinfo",
[SYS_sigalarm]  = "sigalarm",
[SYS_sigreturn] = "sigreturn",
[SYS_symlink]   = "symlink",
[SYS_mmap]      = "mmap",
[SYS_munmap]    = "munmap",
[SYS_memsnapshot] = "memsnapshot",
};

// 名称表中可访问的元素数量（包含下标 0），用于限制遍历和查找范围。
#define SYSCALL_NAME_COUNT (sizeof(syscall_names) / sizeof(syscall_names[0]))

#endif
