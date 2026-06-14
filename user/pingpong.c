#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int p2c[2];
  int c2p[2];
  char byte;

  if(argc != 1){
    fprintf(2, "usage: pingpong\n");
    exit(1);
  }

  pipe(p2c);
  pipe(c2p);

  if(fork() == 0){
    close(p2c[1]);
    close(c2p[0]);
    read(p2c[0], &byte, 1);
    close(p2c[0]);
    printf("%d: received ping\n", getpid());
    write(c2p[1], &byte, 1);
    close(c2p[1]);
    exit(0);
  }

  close(p2c[0]);
  close(c2p[1]);
  byte = 'x';
  write(p2c[1], &byte, 1);
  close(p2c[1]);
  read(c2p[0], &byte, 1);
  close(c2p[0]);
  printf("%d: received pong\n", getpid());
  wait(0);
  exit(0);
}
