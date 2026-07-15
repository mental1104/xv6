#ifndef XV6_SYSCALL_NAMES_H
#define XV6_SYSCALL_NAMES_H

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
};

#define SYSCALL_NAME_COUNT (sizeof(syscall_names) / sizeof(syscall_names[0]))

#endif
