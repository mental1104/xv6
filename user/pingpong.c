#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char** argv){
    int p[2];
    int pid;
    char content;
    if(argc>1){
        printf("Usage: pingpong\n");
        exit(1);
    }
    pipe(p);
    if(fork()==0){
        pid = getpid();
        read(p[0],&content,1);
        close(p[0]);
        printf("%d: received ping\n", pid);
        write(p[1], "0", 1);
        close(p[1]);
    }else{
        pid = getpid();
        write(p[1],"0",1);
        close(p[1]);

        wait(0);

        read(p[0],&content,1);
        close(p[0]);
        printf("%d: received pong\n", pid);
    }
    exit(0);
}