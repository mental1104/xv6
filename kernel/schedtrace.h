#ifndef XV6_SCHEDTRACE_H
#define XV6_SCHEDTRACE_H

#include "schedtrace_abi.h"

struct proc;

void schedtrace_init(void);
void schedtrace_reset(void);
int schedtrace_start(void);
int schedtrace_stop(void);
int schedtrace_watch_pid(int pid);
int schedtrace_copy_snapshot(struct schedtrace_snapshot *snapshot, int max_events);
void schedtrace_record_start(struct proc *p);
void schedtrace_record_stop(struct proc *p, int reason);
void schedtrace_record_boost(unsigned long scheduler_clock, unsigned long epoch);

#endif
