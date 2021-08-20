#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int 
main(int argc, char *argv[])
{
    int duration;
    if(argc<=1){
        fprintf(2,"Require more parameters!");
        exit(1);
    }
    duration = atoi(argv[1]);
    sleep(duration);
    exit(0);
}