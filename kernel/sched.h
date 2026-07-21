#ifndef XV6_SCHED_H
#define XV6_SCHED_H

struct proc;
struct sched_stats;

void schedinit(void);
void sched_timer_yield(void);
int sched_set_hint(int ticks);
int sched_set_weight(int weight);
int sched_get_stats(struct sched_stats *stats);
const char *sched_policy_name(void);

#endif
