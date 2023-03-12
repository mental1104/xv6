#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
void children_process(int p[2]){
    int prime;
    int pp[2];
    int num;
    int len;//to figure out the return val of read;

    close(p[1]);
    len = read(p[0],&prime,sizeof(int));
    if(len == 0){
        close(p[0]);
        exit(0);
    }
    printf("prime %d\n", prime);
    pipe(pp);

    if(fork()==0){
        close(p[0]);
        children_process(pp);
    }else{
        close(pp[0]);
        while(1){
            len = read(p[0],&num,sizeof(int));
            if(len == 0)
                break;
            
            if(num%prime !=0)
                write(pp[1],&num,sizeof(int));
        }
        close(p[0]);
        close(pp[1]);
        wait(0);
    }
    exit(0);
}

int main(int argc, char** argv){
    if(argc>1){
        fprintf(2, "Usage: primes");
        exit(1);
    }

    int i;
    int p[2];
    pipe(p);

    if(fork()==0){
        children_process(p);
    }else{
        close(p[0]);
        for(i = 2;i<=35;i++)
            write(p[1],&i,sizeof(int));
        close(p[1]);
        wait(0);
    }
    exit(0);
}