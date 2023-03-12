#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

char*
fmtname(char *path)
{
  char *p;
  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;
  return p;
}

void find(char* path, char* filename){
    char buf[512],*p;
    int fd;
    struct dirent de;
    struct stat st;
    if((fd = open(path, 0)) < 0){
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }//to open the file.

    if(fstat(fd, &st) < 0){
        fprintf(2, "find: connot stat %s\n", path);
        close(fd);
        return;
    }//to obtain some information about the file.  

    switch(st.type){
        case T_FILE:
            if(strcmp(fmtname(path),filename) == 0)
                printf("%s\n",path);
            break;
            
        case T_DIR:
            if(strlen(path)+1+DIRSIZ+1 > sizeof buf){
                printf("ls: path too long");
                break;
            }
            strcpy(buf, path);
            p = buf+strlen(buf);
            *p++ = '/';

            while(read(fd, &de,sizeof(de)) == sizeof(de)){
                if(de.inum == 0 || strcmp(de.name,".") == 0 || strcmp(de.name,"..") == 0)
                    continue;
                memmove(p, de.name, DIRSIZ);
                p[DIRSIZ] = 0;
                find(buf,filename);
            }
            break;

    }
    close(fd);
}

int main(int argc, char** argv){
    if(argc!=3){
        fprintf(2,"Usage: find directory filename.\n");
        exit(1);
    }
    find(argv[1], argv[2]);
    exit(0);
}