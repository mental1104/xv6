#define NPROC        64  // maximum number of processes
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS 540  // 4 GiB unlink may dirty every bitmap block in one transaction
#define LOGSIZE     600  // total on-disk log region; includes header blocks
#define NBUF        (LOGSIZE + MAXOPBLOCKS) // pinned log blocks plus active I/O
#ifndef FSSIZE
#define FSSIZE   200000  // default file-system image size in blocks
#endif
#define MAXPATH     128  // maximum file path name
#define USERSTACK     1  // user stack pages
