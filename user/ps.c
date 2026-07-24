#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/procinfo.h"
#include "user/user.h"

// 固定进程表最多 NPROC 项；放在 BSS 避免占用 xv6 单页用户栈。
static struct procinfo processes[NPROC];

/**
 * 返回公开进程状态对应的稳定显示文本。
 *
 * @param state PROCINFO_STATE_* 状态值。
 * @return 静态状态名称；未知值返回 "unknown"。
 */
static char *
state_name(int state)
{
  switch(state){
  case PROCINFO_STATE_USED:
    return "used";
  case PROCINFO_STATE_SLEEPING:
    return "sleeping";
  case PROCINFO_STATE_RUNNABLE:
    return "runnable";
  case PROCINFO_STATE_RUNNING:
    return "running";
  case PROCINFO_STATE_STOPPED:
    return "stopped";
  case PROCINFO_STATE_ZOMBIE:
    return "zombie";
  default:
    return "unknown";
  }
}

/**
 * 按 PID 升序整理快照，使槽位复用不会让命令输出顺序随机变化。
 *
 * @param entries 待排序的进程快照数组。
 * @param count 有效条目数量。
 */
static void
sort_by_pid(struct procinfo *entries, int count)
{
  for(int i = 1; i < count; i++){
    struct procinfo current = entries[i];
    int position = i;

    while(position > 0 && entries[position - 1].pid > current.pid){
      entries[position] = entries[position - 1];
      position--;
    }
    entries[position] = current;
  }
}

/** 打印当前全部非 UNUSED 进程的结构化快照。 */
int
main(int argc, char **argv)
{
  int count;

  if(argc != 1){
    fprintf(2, "Usage: ps\n");
    exit(2);
  }

  count = getprocs(processes, NPROC);
  if(count < 0){
    fprintf(2, "ps: cannot read process table\n");
    exit(1);
  }

  sort_by_pid(processes, count);
  printf("PID\tPPID\tSTATE\tNAME\n");
  for(int i = 0; i < count; i++)
    printf("%d\t%d\t%s\t%s\n",
           processes[i].pid,
           processes[i].ppid,
           state_name(processes[i].state),
           processes[i].name);

  exit(0);
}
