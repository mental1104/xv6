// Shell.

#include "kernel/types.h"
#include "user/user.h"
#include "user/history.h"
#include "user/shellinput.h"
#include "kernel/fcntl.h"
#include "kernel/wait.h"
#include "kernel/jobctl.h"

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10
#define MAXJOBS 16
#define MAXCMD  100

/** Shell 只跟踪顶层 supervisor；其 pipeline 后代通过 PGID 统一受控。 */
enum jobstate { JOB_UNUSED, JOB_RUNNING, JOB_STOPPED };

/** 保存一个由当前 Shell 直接创建并负责回收的作业。 */
struct job {
  int jid;
  int pid;
  int pgid;
  enum jobstate state;
  char command[MAXCMD];
};

static struct job jobs[MAXJOBS];
static int nextjid = 1;
static int shell_pgid;
static struct shell_history command_history;

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

int fork1(void);  // Fork but panics on failure.
void panic(char*);
struct cmd *parsecmd(char*);
void runline(struct cmd*);
void runcmd(struct cmd*) __attribute__((noreturn));

/** 将字符串复制到固定容量缓冲区并保证 NUL 结尾。 */
static void
copytext(char *dst, char *src, int size)
{
  int i;

  for(i = 0; i + 1 < size && src[i]; i++)
    dst[i] = src[i];
  dst[i] = 0;
}

/** 向格式化命令缓冲区追加文本，超出容量时保留已写部分。 */
static void
appendtext(char *dst, int *length, int size, char *text)
{
  while(*text && *length + 1 < size)
    dst[(*length)++] = *text++;
  dst[*length] = 0;
}

/** 将命令树压缩成 jobs 可显示的单行文本。 */
static void
formatcmd(struct cmd *cmd, char *dst, int *length, int size)
{
  int i;

  switch(cmd->type){
  case EXEC: {
    struct execcmd *ecmd = (struct execcmd*)cmd;
    for(i = 0; ecmd->argv[i]; i++){
      if(i > 0)
        appendtext(dst, length, size, " ");
      appendtext(dst, length, size, ecmd->argv[i]);
    }
    break;
  }
  case REDIR: {
    struct redircmd *rcmd = (struct redircmd*)cmd;
    formatcmd(rcmd->cmd, dst, length, size);
    if(rcmd->fd == 0)
      appendtext(dst, length, size, " < ");
    else if(rcmd->mode & O_TRUNC)
      appendtext(dst, length, size, " > ");
    else
      appendtext(dst, length, size, " >> ");
    appendtext(dst, length, size, rcmd->file);
    break;
  }
  case PIPE: {
    struct pipecmd *pcmd = (struct pipecmd*)cmd;
    formatcmd(pcmd->left, dst, length, size);
    appendtext(dst, length, size, " | ");
    formatcmd(pcmd->right, dst, length, size);
    break;
  }
  case LIST: {
    struct listcmd *lcmd = (struct listcmd*)cmd;
    formatcmd(lcmd->left, dst, length, size);
    appendtext(dst, length, size, "; ");
    formatcmd(lcmd->right, dst, length, size);
    break;
  }
  case BACK:
    formatcmd(((struct backcmd*)cmd)->cmd, dst, length, size);
    appendtext(dst, length, size, " &");
    break;
  }
}

/** 清空作业槽位；JID 不复用，避免旧引用命中新作业。 */
static void
clearjob(struct job *job)
{
  memset(job, 0, sizeof(*job));
}

/** 根据 JID 查找仍被当前 Shell 跟踪的运行或停止作业。 */
static struct job*
findjob(int jid)
{
  struct job *job;

  for(job = jobs; job < &jobs[MAXJOBS]; job++)
    if(job->state != JOB_UNUSED && job->jid == jid)
      return job;
  return 0;
}

/** 返回空闲作业槽位；找不到时返回 0。 */
static struct job*
emptyjob(void)
{
  struct job *job;

  for(job = jobs; job < &jobs[MAXJOBS]; job++)
    if(job->state == JOB_UNUSED)
      return job;
  return 0;
}

/** 将 supervisor PID/PGID 登记为一个可由 jobs、fg、bg 操作的作业。 */
static struct job*
recordjob(int pid, int pgid, enum jobstate state, char *command)
{
  struct job *job = emptyjob();

  if(job == 0){
    fprintf(2, "sh: too many jobs\n");
    return 0;
  }
  job->jid = nextjid++;
  job->pid = pid;
  job->pgid = pgid;
  job->state = state;
  copytext(job->command, command, sizeof(job->command));
  return job;
}

/** 返回作业状态的人类可读名称。 */
static char*
jobstatename(enum jobstate state)
{
  return state == JOB_STOPPED ? "Stopped" : "Running";
}

/**
 * 非阻塞消费后台作业的 EXITED、STOPPED 和 CONTINUED 事件。
 *
 * 每个事件由内核只返回一次；退出时清理作业槽，停止/继续只更新状态。快速退出和
 * 多个后台作业交错完成不会让 Shell 阻塞在提示符安全点。
 */
static void
reapjobs(void)
{
  struct job *job;

  for(job = jobs; job < &jobs[MAXJOBS]; job++){
    if(job->state == JOB_UNUSED)
      continue;

    for(;;){
      int status = 0;
      int pid = waitpid(job->pid, &status,
                        WNOHANG | WUNTRACED | WCONTINUED);
      if(pid == 0)
        break;
      if(pid < 0){
        clearjob(job);
        break;
      }
      if(WIFSTOPPED(status)){
        if(job->state != JOB_STOPPED)
          printf("[%d] Stopped %s\n", job->jid, job->command);
        job->state = JOB_STOPPED;
        continue;
      }
      if(WIFCONTINUED(status)){
        job->state = JOB_RUNNING;
        continue;
      }
      printf("[%d] Done %s\n", job->jid, job->command);
      clearjob(job);
      break;
    }
  }
}

/**
 * 创建后台 supervisor 并让父子双方尝试设置 PGID，缩小 fork/exec 启动竞态。
 */
static void
startjob(struct cmd *cmd, char *command)
{
  int pid;
  struct job *job;

  reapjobs();
  if(emptyjob() == 0){
    fprintf(2, "sh: too many background jobs\n");
    return;
  }

  pid = fork1();
  if(pid == 0){
    if(setpgid(0, 0) < 0)
      exit(1);
    runcmd(cmd);
  }

  if(setpgid(pid, pid) < 0){
    kill(pid);
    waitpid(pid, 0, 0);
    fprintf(2, "sh: cannot create process group\n");
    return;
  }
  job = recordjob(pid, pid, JOB_RUNNING, command);
  if(job == 0){
    procctl(pid, JOBCTL_TERM);
    waitpid(pid, 0, 0);
    return;
  }
  printf("[%d] %d\n", job->jid, job->pgid);
}

/** 输出当前运行和停止作业；展示值以 PGID 为统一控制标识。 */
static void
showjobs(void)
{
  struct job *job;

  reapjobs();
  for(job = jobs; job < &jobs[MAXJOBS]; job++)
    if(job->state != JOB_UNUSED)
      printf("[%d] %d %s %s\n", job->jid, job->pgid,
             jobstatename(job->state), job->command);
}

/**
 * 将作业交给控制台前台并等待 supervisor 停止或退出。
 *
 * @param job 已登记后台作业；新前台命令传 0。
 * @param pid Shell 直接子进程 supervisor PID。
 * @param pgid pipeline 全体成员共享的进程组 ID。
 * @param command 用于前台命令被停止后创建 jobs 条目的文本。
 */
static void
waitforeground(struct job *job, int pid, int pgid, char *command)
{
  int status = 0;

  if(tcsetpgrp(pgid) < 0){
    fprintf(2, "sh: cannot give console to pgid %d\n", pgid);
    return;
  }

  if(job != 0 && job->state == JOB_STOPPED){
    if(procctl(pgid, JOBCTL_CONT) < 0){
      fprintf(2, "fg: cannot continue job %d\n", job->jid);
      tcsetpgrp(shell_pgid);
      return;
    }
    job->state = JOB_RUNNING;
  }

  for(;;){
    int waited = waitpid(pid, &status, WUNTRACED | WCONTINUED);
    if(waited < 0){
      fprintf(2, "sh: wait failed for pgid %d\n", pgid);
      break;
    }
    if(WIFCONTINUED(status)){
      if(job != 0)
        job->state = JOB_RUNNING;
      continue;
    }
    if(WIFSTOPPED(status)){
      if(job == 0)
        job = recordjob(pid, pgid, JOB_STOPPED, command);
      else
        job->state = JOB_STOPPED;
      if(job != 0)
        printf("[%d] Stopped %s\n", job->jid, job->command);
      break;
    }
    if(job != 0)
      clearjob(job);
    break;
  }

  if(tcsetpgrp(shell_pgid) < 0)
    fprintf(2, "sh: cannot reclaim console\n");
}

/** 解析 `%jid` 或纯数字 JID；非法输入返回 -1。 */
static int
parsejid(char *s)
{
  int jid = 0;

  while(*s == ' ' || *s == '\t')
    s++;
  if(*s == '%')
    s++;
  if(*s < '0' || *s > '9')
    return -1;
  while(*s >= '0' && *s <= '9'){
    jid = jid * 10 + *s - '0';
    s++;
  }
  while(*s == ' ' || *s == '\t')
    s++;
  if(*s != 0 || jid == 0)
    return -1;
  return jid;
}

/** 将停止或运行的后台作业切换到前台并等待下一次状态边界。 */
static void
foreground(char *arg)
{
  int jid;
  struct job *job;

  if((jid = parsejid(arg)) < 0 || (job = findjob(jid)) == 0){
    fprintf(2, "fg: no such job\n");
    return;
  }
  waitforeground(job, job->pid, job->pgid, job->command);
}

/** 恢复停止作业到后台并立即返回提示符。 */
static void
background(char *arg)
{
  int jid;
  struct job *job;

  if((jid = parsejid(arg)) < 0 || (job = findjob(jid)) == 0){
    fprintf(2, "bg: no such job\n");
    return;
  }
  if(job->state != JOB_STOPPED){
    fprintf(2, "bg: job %d is already running\n", jid);
    return;
  }
  if(procctl(job->pgid, JOBCTL_CONT) < 0){
    fprintf(2, "bg: cannot continue job %d\n", jid);
    return;
  }
  job->state = JOB_RUNNING;
  printf("[%d] %d Running %s\n", job->jid, job->pgid, job->command);
}

/** 按从旧到新的顺序输出当前 Shell 进程拥有的会话历史。 */
static void
showhistory(void)
{
  for(int i = 0; i < shell_history_count(&command_history); i++){
    const struct shell_history_entry *entry = shell_history_at(&command_history, i);
    printf("%d %s\n", entry->number, entry->command);
  }
}

/** 判断字符串是否以前缀开头。 */
static int
startswith(char *s, char *prefix)
{
  while(*prefix && *s == *prefix){
    s++;
    prefix++;
  }
  return *prefix == 0;
}

/**
 * 在 Shell 父进程内分派需要持久状态或影响当前 Shell 的内置命令。
 *
 * @param buf 已去除行尾的完整命令。
 * @return 命中内置命令并完成处理时返回 1，否则返回 0。
 */
static int
runbuiltin(char *buf)
{
  if(strcmp(buf, "history") == 0){
    showhistory();
    return 1;
  }
  if(strcmp(buf, "jobs") == 0){
    showjobs();
    return 1;
  }
  if(startswith(buf, "fg ")){
    foreground(buf + 3);
    return 1;
  }
  if(startswith(buf, "bg ")){
    background(buf + 3);
    return 1;
  }
  if(startswith(buf, "cd ")){
    if(chdir(buf + 3) < 0)
      fprintf(2, "cannot cd %s\n", buf + 3);
    return 1;
  }
  return 0;
}

__attribute__((noreturn))
// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  int p[2];
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    exit(1);

  switch(cmd->type){
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[0] == 0)
      exit(1);
    exec(ecmd->argv[0], ecmd->argv);
    fprintf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    close(rcmd->fd);
    if(open(rcmd->file, rcmd->mode) < 0){
      fprintf(2, "open %s failed\n", rcmd->file);
      exit(1);
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    if(fork1() == 0)
      runcmd(lcmd->left);
    wait(0);
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    if(pipe(p) < 0)
      panic("pipe");
    if(fork1() == 0){
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if(fork1() == 0){
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait(0);
    wait(0);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    if(fork1() == 0)
      runcmd(bcmd->cmd);
    break;
  }
  exit(0);
}

/**
 * 输出提示符并读取一条命令。
 *
 * @param buf 命令缓冲区。
 * @param nbuf 缓冲区容量。
 * @return 成功提交一行返回 0；EOF、空行 Ctrl-D 或读取失败返回 -1。
 */
int
getcmd(char *buf, int nbuf)
{
  fprintf(2, "$ ");
  memset(buf, 0, nbuf);
  return shell_readline(buf, nbuf, &command_history);
}

int
main(void)
{
  static char buf[100];
  int fd;

  shell_history_init(&command_history);

  // Ensure that three file descriptors are open.
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }

  // Shell 自成进程组，并成为单控制台初始前台 owner。
  if(setpgid(0, 0) < 0 || (shell_pgid = getpgid(0)) < 0 ||
     tcsetpgrp(shell_pgid) < 0){
    fprintf(2, "sh: cannot initialize job control\n");
    exit(1);
  }

  // Read and run input commands.
  for(;;){
    reapjobs();
    if(getcmd(buf, sizeof(buf)) < 0)
      break;
    int len = strlen(buf);
    if(len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
      buf[len-1] = 0;
    if(buf[0] == 0)
      continue;
    // 历史先于内置命令分派更新，保证 `history` 输出包含当前这一条命令。
    shell_history_add(&command_history, buf);
    if(runbuiltin(buf))
      continue;
    runline(parsecmd(buf));
  }
  exit(0);
}

/**
 * 在父 Shell 中展开顶层列表，并为每个前台或后台命令创建独立进程组。
 */
void
runline(struct cmd *cmd)
{
  char command[MAXCMD] = {0};
  int command_length = 0;
  int pid;

  if(cmd->type == LIST){
    struct listcmd *lcmd = (struct listcmd*)cmd;
    runline(lcmd->left);
    runline(lcmd->right);
    return;
  }

  if(cmd->type == BACK){
    formatcmd(((struct backcmd*)cmd)->cmd, command,
              &command_length, sizeof(command));
    appendtext(command, &command_length, sizeof(command), " &");
    startjob(((struct backcmd*)cmd)->cmd, command);
    return;
  }

  formatcmd(cmd, command, &command_length, sizeof(command));
  pid = fork1();
  if(pid == 0){
    if(setpgid(0, 0) < 0)
      exit(1);
    runcmd(cmd);
  }
  if(setpgid(pid, pid) < 0){
    kill(pid);
    waitpid(pid, 0, 0);
    fprintf(2, "sh: cannot create foreground process group\n");
    return;
  }
  waitforeground(0, pid, pid, command);
}

void
panic(char *s)
{
  fprintf(2, "%s\n", s);
  exit(1);
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd*
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd*)cmd;
}

struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
}
//PAGEBREAK!
// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  if(q)
    *q = s;
  ret = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if(*s == '>'){
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if(eq)
    *eq = s;

  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd*
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if(s != es){
    fprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while(peek(ps, es, "&")){
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if(peek(ps, es, ";")){
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd*
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if(peek(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd*
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while(peek(ps, es, "<>")){
    tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch(tok){
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE|O_TRUNC, 1);
      break;
    case '+':  // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd*
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if(!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if(!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd*
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if(peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd*)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while(!peek(ps, es, "|)&;")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    if(tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if(argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd*
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    return 0;

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    for(i=0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}
