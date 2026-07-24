#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/paths.h"

static char output[2048];

static void
fail(char *message)
{
  fprintf(2, "memtargettest: FAIL: %s\n", message);
  exit(1);
}

static int
text_contains(char *text, char *pattern)
{
  for(int i = 0; text[i] != 0; i++){
    int j = 0;
    while(pattern[j] != 0 && text[i + j] == pattern[j])
      j++;
    if(pattern[j] == 0)
      return 1;
  }
  return 0;
}

static int
run_and_capture(char *pages, char *interval, int quiet_stderr)
{
  int fds[2];
  if(pipe(fds) < 0)
    fail("pipe");

  int pid = fork();
  if(pid < 0)
    fail("fork");
  if(pid == 0){
    close(fds[0]);
    close(1);
    if(dup(fds[1]) != 1)
      exit(1);
    close(fds[1]);
    if(quiet_stderr)
      close(2);

    char *argv[] = {
      XV6_USR_BIN_PATH("memtarget"), pages, interval, "--exit", 0
    };
    exec(argv[0], argv);
    exit(1);
  }

  close(fds[1]);
  int total = 0;
  while(total < (int)sizeof(output) - 1){
    int count = read(fds[0], output + total, sizeof(output) - 1 - total);
    if(count < 0)
      fail("read");
    if(count == 0)
      break;
    total += count;
  }
  close(fds[0]);
  output[total] = 0;

  int status = -1;
  if(wait(&status) != pid)
    fail("wait");
  return status;
}

static void
test_lifecycle(void)
{
  if(run_and_capture("2", "1", 0) != 0)
    fail("valid lifecycle exit status");
  if(!text_contains(output, "memtarget: pid=") ||
     !text_contains(output, "page=1/2 state=lazy") ||
     !text_contains(output, "page=1/2 state=resident") ||
     !text_contains(output, "page=2/2 state=lazy") ||
     !text_contains(output, "page=2/2 state=resident") ||
     !text_contains(output, "state=complete pages=2"))
    fail("lifecycle output");

  printf("memtargettest: lifecycle OK\n");
}

static void
test_invalid_arguments(void)
{
  if(run_and_capture("0", "1", 1) == 0)
    fail("zero pages accepted");
  if(run_and_capture("1", "0", 1) == 0)
    fail("zero interval accepted");

  printf("memtargettest: invalid arguments OK\n");
}

int
main(void)
{
  test_lifecycle();
  test_invalid_arguments();
  printf("memtargettest: OK\n");
  exit(0);
}
