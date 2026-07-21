#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/schedstat.h"
#include "user/user.h"

static void
burn(void)
{
  volatile uint64 value = 1;
  for(int i = 0; i < 20000; i++)
    value = value * 1664525 + 1013904223;
}

static void
worker(int id, int hint, int weight, int readyfd, int startfd)
{
  char token = 'r';
  int started;
  struct sched_stats stats;

  if(sched_set_hint(hint) < 0 || sched_set_weight(weight) < 0){
    printf("schedtest: setup failed worker=%d\n", id);
    exit(1);
  }
  write(readyfd, &token, 1);
  if(read(startfd, &token, 1) != 1)
    exit(1);

  started = uptime();
  while(uptime() - started < hint)
    burn();

  if(sched_get_stats(&stats) < 0)
    exit(1);
  printf("schedtest worker=%d pid=%d policy=%d runtime=%d dispatch=%d "
         "hint=%d remain=%d level=%d weight=%d vruntime=%d\n",
         id, stats.pid, stats.policy, (int)stats.runtime_ticks,
         (int)stats.dispatches, (int)stats.burst_hint,
         (int)stats.remaining_hint, stats.mlfq_level, stats.weight,
         (int)stats.vruntime);
  exit(0);
}

int
main(void)
{
  int ready[2];
  int start[2];
  int hints[] = {8, 2, 5};
  int weights[] = {1024, 2048, 512};
  char token;

  if(pipe(ready) < 0 || pipe(start) < 0){
    printf("schedtest: pipe failed\n");
    exit(1);
  }

  for(int i = 0; i < 3; i++){
    int pid = fork();
    if(pid < 0){
      printf("schedtest: fork failed\n");
      exit(1);
    }
    if(pid == 0){
      close(ready[0]);
      close(start[1]);
      worker(i, hints[i], weights[i], ready[1], start[0]);
    }
  }

  close(ready[1]);
  close(start[0]);
  for(int i = 0; i < 3; i++)
    if(read(ready[0], &token, 1) != 1)
      exit(1);
  for(int i = 0; i < 3; i++)
    write(start[1], "s", 1);

  close(ready[0]);
  close(start[1]);
  for(int i = 0; i < 3; i++)
    wait(0);

  printf("schedtest: OK\n");
  exit(0);
}
