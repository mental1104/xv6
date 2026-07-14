#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

static void
run_pipeline(int input[2])
{
  int read_fd = input[0];
  close(input[1]);

  for(;;){
    int prime;
    if(read(read_fd, &prime, sizeof(prime)) != sizeof(prime)){
      close(read_fd);
      exit(0);
    }

    printf("prime %d\n", prime);

    int next[2];
    if(pipe(next) < 0){
      close(read_fd);
      exit(1);
    }

    int pid = fork();
    if(pid < 0){
      close(read_fd);
      close(next[0]);
      close(next[1]);
      exit(1);
    }

    if(pid == 0){
      close(read_fd);
      close(next[1]);
      read_fd = next[0];
      continue;
    }

    close(next[0]);

    int num;
    while(read(read_fd, &num, sizeof(num)) == sizeof(num)){
      if(num % prime != 0)
        write(next[1], &num, sizeof(num));
    }

    close(read_fd);
    close(next[1]);
    wait(0);
    exit(0);
  }
}

int
main(int argc, char **argv)
{
  if(argc != 1){
    fprintf(2, "usage: primes\n");
    exit(1);
  }

  int input[2];
  if(pipe(input) < 0)
    exit(1);

  int pid = fork();
  if(pid < 0)
    exit(1);

  if(pid == 0)
    run_pipeline(input);

  close(input[0]);
  for(int i = 2; i <= 35; i++)
    write(input[1], &i, sizeof(i));
  close(input[1]);
  wait(0);
  exit(0);
}
