#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"

/**
 * 验证 inode 能映射并读回 MAXFILE 个数据块，同时拒绝第 MAXFILE + 1 个块。
 *
 * 测试使用由 fs.h 定义的 MAXFILE 作为唯一边界，避免文件系统配置或寻址布局
 * 变化后继续无限写入，最终以磁盘耗尽或 QEMU 超时结束。
 *
 * @return 成功时调用 exit(0)；任一写入、边界或读回校验失败时调用 exit(-1)。
 */
int
main(void)
{
  char buf[BSIZE] = {0};
  int fd, i, blocks;
  const int max_blocks = (int)MAXFILE;

  fd = open("big.file", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf("bigfile: cannot open big.file for writing\n");
    exit(-1);
  }

  // 只写入寻址布局明确允许的块数，避免依赖“持续写到失败”终止测试。
  for(blocks = 0; blocks < max_blocks; blocks++){
    *(int*)buf = blocks;
    int cc = write(fd, buf, BSIZE);
    if(cc != BSIZE){
      printf("bigfile: write error at block %d\n", blocks);
      exit(-1);
    }
    if((blocks + 1) % 100 == 0)
      printf(".");
  }

  // writei 必须在 MAXFILE * BSIZE 边界拒绝写入，且不能推进文件偏移。
  *(int*)buf = max_blocks;
  if(write(fd, buf, BSIZE) >= 0){
    printf("bigfile: accepted block %d beyond MAXFILE\n", max_blocks);
    exit(-1);
  }

  printf("\nwrote %d blocks\n", blocks);
  close(fd);

  fd = open("big.file", O_RDONLY);
  if(fd < 0){
    printf("bigfile: cannot re-open big.file for reading\n");
    exit(-1);
  }
  for(i = 0; i < blocks; i++){
    int cc = read(fd, buf, BSIZE);
    if(cc != BSIZE){
      printf("bigfile: read error at block %d\n", i);
      exit(-1);
    }
    if(*(int*)buf != i){
      printf("bigfile: read the wrong data (%d) for block %d\n",
             *(int*)buf, i);
      exit(-1);
    }
  }

  close(fd);
  printf("bigfile done; ok\n");
  exit(0);
}
