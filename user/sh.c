// Shell.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/wait.h"

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10
#define MAXJOBS 16
#define MAXCMD  100

enum jobstate { JOB_UNUSED, JOB_RUNNING };

struct job {
  int jid;
  int pid;
  enum jobstate state;
  char command[MAXCMD];
};

static struct job jobs[MAXJOBS];
static int nextjid = 1;

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

static void
copytext(char *dst, char *src, int size)
{
  int i;

  for(i = 0; i + 1 < size && src[i]; i++)
    dst[i] = src[i];
  dst[i] = 0;
}

static void
appendtext(char *dst, int *length, int size, char *text)
{
  while(*text && *length + 1 < size)
    dst[(*length)++] = *text++;
  dst[*length] = 0;
}

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

static void
clearjob(struct job *job)
{
  memset(job, 0, sizeof(*job));
}

static struct job*
findjob(int jid)
{
  struct job *job;

  for(job = jobs; job < &jobs[MAXJOBS]; job++)
    if(job->state == JOB_RUNNING && job->jid == jid)
      return job;
  return 0;
}

static void
reapjobs(void)
{
  struct job *job;
  int status;

  for(job = jobs; job < &jobs[MAXJOBS]; job++){
    if(job->state != JOB_RUNNING)
      continue;
    int pid = waitpid(job->pid, &status, WNOHANG);
    if(pid == job->pid){
      printf("[%d] Done %s\n", job->jid, job->command);
      clearjob(job);
    } else if(pid < 0){
      clearjob(job);
    }
  }
}

static struct job*
emptyjob(void)
{
  struct job *job;

  for(job = jobs; job < &jobs[MAXJOBS]; job++)
    if(job->state == JOB_UNUSED)
      return job;
  return 0;
}

static void
startjob(struct cmd *cmd, char *command)
{
  struct job *job;
  int pid;

  reapjobs();
  if((job = emptyjob()) == 0){
    fprintf(2, "sh: too many background jobs\n");
    return;
  }

  pid = fork1();
  if(pid == 0)
    runcmd(cmd);

  job->jid = nextjid++;
  job->pid = pid;
  job->state = JOB_RUNNING;
  copytext(job->command, command, sizeof(job->command));
  printf("[%d] %d\n", job->jid, job->pid);
}

static void
showjobs(void)
{
  struct job *job;

  reapjobs();
  for(job = jobs; job < &jobs[MAXJOBS]; job++)
    if(job->state == JOB_RUNNING)
      printf("[%d] %d Running %s\n", job->jid, job->pid, job->command);
}

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

static void
foreground(char *arg)
{
  int jid, status;
  struct job *job;

  if((jid = parsejid(arg)) < 0 || (job = findjob(jid)) == 0){
    fprintf(2, "fg: no such job\n");
    return;
  }
  if(waitpid(job->pid, &status, 0) < 0){
    fprintf(2, "fg: wait failed for job %d\n", jid);
    return;
  }
  clearjob(job);
}

static int
startswith(char *s, char *prefix)
{
  while(*prefix && *s == *prefix){
    s++;
    prefix++;
  }
  return *prefix == 0;
}

static int
runbuiltin(char *buf)
{
  if(strcmp(buf, "jobs") == 0){
    showjobs();
    return 1;
  }
  if(startswith(buf, "fg ")){
    foreground(buf + 3);
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

int
getcmd(char *buf, int nbuf)
{
  fprintf(2, "$ ");
  memset(buf, 0, nbuf);
  gets(buf, nbuf);
  if(buf[0] == 0) // EOF
    return -1;
  return 0;
}

int
main(void)
{
  static char buf[100];
  int fd;

  // Ensure that three file descriptors are open.
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
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
    if(runbuiltin(buf))
      continue;
    runline(parsecmd(buf));
  }
  exit(0);
}

// Run lists in the shell so every top-level background command remains
// a direct child that can be tracked and reaped by this shell.
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

  pid = fork1();
  if(pid == 0)
    runcmd(cmd);
  if(waitpid(pid, 0, 0) < 0)
    fprintf(2, "sh: wait failed\n");
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
