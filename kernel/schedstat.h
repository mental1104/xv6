#ifndef XV6_SCHEDSTAT_H
#define XV6_SCHEDSTAT_H

#define SCHED_POLICY_RR    0
#define SCHED_POLICY_FIFO  1
#define SCHED_POLICY_SJF   2
#define SCHED_POLICY_STCF  3
#define SCHED_POLICY_MLFQ  4
#define SCHED_POLICY_CFS   5

struct sched_stats {
  int policy;
  int pid;
  int mlfq_level;
  int weight;
  unsigned long runtime_ticks;
  unsigned long dispatches;
  unsigned long burst_hint;
  unsigned long remaining_hint;
  unsigned long vruntime;
};

#endif
