#include "kernel/param.h"
#include "kernel/fcntl.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "kernel/fs.h"
#include "user/user.h"

void mmap_test();
void fork_test();
void vma_limit_test();
char buf[BSIZE];

#define MAP_FAILED ((char *) -1)

/**
 * 依次运行 mmap 基础行为、VMA 配额和 fork 继承回归。
 *
 * @param argc 用户程序参数数量，本测试不使用。
 * @param argv 用户程序参数数组，本测试不使用。
 * @return 测试全部通过时以状态 0 退出；任一断言失败时由 err() 以状态 1 退出。
 */
int
main(int argc, char *argv[])
{
  mmap_test();
  vma_limit_test();
  fork_test();
  printf("mmaptest: all tests succeeded\n");
  exit(0);
}

char *testname = "???";

void
err(char *why)
{
  printf("mmaptest: %s failed: %s, pid=%d\n", testname, why, getpid());
  exit(1);
}

//
// check the content of the two mapped pages.
//
void
_v1(char *p)
{
  int i;
  for (i = 0; i < PGSIZE*2; i++) {
    if (i < PGSIZE + (PGSIZE/2)) {
      if (p[i] != 'A') {
        printf("mismatch at %d, wanted 'A', got 0x%x\n", i, p[i]);
        err("v1 mismatch (1)");
      }
    } else {
      if (p[i] != 0) {
        printf("mismatch at %d, wanted zero, got 0x%x\n", i, p[i]);
        err("v1 mismatch (2)");
      }
    }
  }
}

//
// create a file to be mapped, containing
// 1.5 pages of 'A' and half a page of zeros.
//
void
makefile(const char *f)
{
  int i;
  int n = PGSIZE/BSIZE;

  unlink(f);
  int fd = open(f, O_WRONLY | O_CREATE);
  if (fd == -1)
    err("open");
  memset(buf, 'A', BSIZE);
  // write 1.5 page
  for (i = 0; i < n + n/2; i++) {
    if (write(fd, buf, BSIZE) != BSIZE)
      err("write 0 makefile");
  }
  if (close(fd) == -1)
    err("close");
}

void
mmap_test(void)
{
  int fd;
  int i;
  const char * const f = "mmap.dur";
  printf("mmap_test starting\n");
  testname = "mmap_test";

  //
  // create a file with known content, map it into memory, check that
  // the mapped memory has the same bytes as originally written to the
  // file.
  //
  makefile(f);
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open");

  printf("test mmap f\n");
  //
  // this call to mmap() asks the kernel to map the content
  // of open file fd into the address space. the first
  // 0 argument indicates that the kernel should choose the
  // virtual address. the second argument indicates how many
  // bytes to map. the third argument indicates that the
  // mapped memory should be read-only. the fourth argument
  // indicates that, if the process modifies the mapped memory,
  // that the modifications should not be written back to
  // the file nor shared with other processes mapping the
  // same file (of course in this case updates are prohibited
  // due to PROT_READ). the fifth argument is the file descriptor
  // of the file to be mapped. the last argument is the starting
  // offset in the file.
  //
  char *p = mmap(0, PGSIZE*2, PROT_READ, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (1)");
  _v1(p);
  if (munmap(p, PGSIZE*2) == -1)
    err("munmap (1)");

  printf("test mmap f: OK\n");

  printf("test mmap private\n");
  // should be able to map file opened read-only with private writable
  // mapping
  p = mmap(0, PGSIZE*2, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (2)");
  if (close(fd) == -1)
    err("close");
  _v1(p);
  for (i = 0; i < PGSIZE*2; i++)
    p[i] = 'Z';
  if (munmap(p, PGSIZE*2) == -1)
    err("munmap (2)");

  printf("test mmap private: OK\n");

  printf("test mmap read-only\n");

  // check that mmap doesn't allow read/write mapping of a
  // file opened read-only.
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open");
  p = mmap(0, PGSIZE*3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p != MAP_FAILED)
    err("mmap call should have failed");
  if (close(fd) == -1)
    err("close");

  printf("test mmap read-only: OK\n");

  printf("test mmap read/write\n");

  // check that mmap does allow read/write mapping of a
  // file opened read/write.
  if ((fd = open(f, O_RDWR)) == -1)
    err("open");
  p = mmap(0, PGSIZE*3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (3)");
  if (close(fd) == -1)
    err("close");

  // check that the mapping still works after close(fd).
  _v1(p);

  // write the mapped memory.
  for (i = 0; i < PGSIZE*2; i++)
    p[i] = 'Z';

  // unmap just the first two of three pages of mapped memory.
  if (munmap(p, PGSIZE*2) == -1)
    err("munmap (3)");

  printf("test mmap read/write: OK\n");

  printf("test mmap dirty\n");

  // check that the writes to the mapped memory were
  // written to the file.
  if ((fd = open(f, O_RDWR)) == -1)
    err("open");
  for (i = 0; i < PGSIZE + (PGSIZE/2); i++){
    char b;
    if (read(fd, &b, 1) != 1)
      err("read (1)");
    if (b != 'Z')
      err("file does not contain modifications");
  }
  if (close(fd) == -1)
    err("close");

  printf("test mmap dirty: OK\n");

  printf("test not-mapped unmap\n");

  // unmap the rest of the mapped memory.
  if (munmap(p+PGSIZE*2, PGSIZE) == -1)
    err("munmap (4)");

  printf("test not-mapped unmap: OK\n");

  printf("test mmap two files\n");

  //
  // mmap two files at the same time.
  //
  int fd1;
  if((fd1 = open("mmap1", O_RDWR|O_CREATE)) < 0)
    err("open mmap1");
  if(write(fd1, "12345", 5) != 5)
    err("write mmap1");
  char *p1 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd1, 0);
  if(p1 == MAP_FAILED)
    err("mmap mmap1");
  close(fd1);
  unlink("mmap1");

  int fd2;
  if((fd2 = open("mmap2", O_RDWR|O_CREATE)) < 0)
    err("open mmap2");
  if(write(fd2, "67890", 5) != 5)
    err("write mmap2");
  char *p2 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd2, 0);
  if(p2 == MAP_FAILED)
    err("mmap mmap2");
  close(fd2);
  unlink("mmap2");

  if(memcmp(p1, "12345", 5) != 0)
    err("mmap1 mismatch");
  if(memcmp(p2, "67890", 5) != 0)
    err("mmap2 mismatch");

  munmap(p1, PGSIZE);
  if(memcmp(p2, "67890", 5) != 0)
    err("mmap2 mismatch (2)");
  munmap(p2, PGSIZE);

  printf("test mmap two files: OK\n");

  printf("mmap_test: ALL OK\n");
}

//
// mmap a file, then fork.
// check that the child sees the mapped file.
//
void
fork_test(void)
{
  int fd;
  int pid;
  const char * const f = "mmap.dur";

  printf("fork_test starting\n");
  testname = "fork_test";

  // mmap the file twice.
  makefile(f);
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open");
  unlink(f);
  char *p1 = mmap(0, PGSIZE*2, PROT_READ, MAP_SHARED, fd, 0);
  if (p1 == MAP_FAILED)
    err("mmap (4)");
  char *p2 = mmap(0, PGSIZE*2, PROT_READ, MAP_SHARED, fd, 0);
  if (p2 == MAP_FAILED)
    err("mmap (5)");

  // read just 2nd page.
  if(*(p1+PGSIZE) != 'A')
    err("fork mismatch (1)");

  if((pid = fork()) < 0)
    err("fork");
  if (pid == 0) {
    _v1(p1);
    munmap(p1, PGSIZE); // just the first page
    exit(0); // tell the parent that the mapping looks OK.
  }

  int status = -1;
  wait(&status);

  if(status != 0){
    printf("fork_test failed\n");
    exit(1);
  }

  // check that the parent's mappings are still there.
  _v1(p1);
  _v1(p2);

  printf("fork_test OK\n");
}

/**
 * 使用同一个文件填满当前进程的全部 VMA 槽位。
 *
 * @param fd 已打开且可读的文件描述符；函数不会关闭它。
 * @param mappings 输出数组，成功后每一项保存一个单页映射地址。
 */
static void
map_all_vma_slots(int fd, char **mappings)
{
  for(int i = 0; i < NOFILE; i++){
    mappings[i] = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    if(mappings[i] == MAP_FAILED)
      err("mmap before reaching NOFILE");
  }
}

/**
 * 解除 map_all_vma_slots() 创建的全部映射并归还 VMA 描述符。
 *
 * @param mappings 包含 NOFILE 个有效单页映射地址的数组。
 */
static void
unmap_all_vma_slots(char **mappings)
{
  for(int i = 0; i < NOFILE; i++){
    if(munmap(mappings[i], PGSIZE) < 0)
      err("munmap VMA slot");
  }
}

/**
 * 验证 VMA 配额属于单个进程，并检查 fork 继承与描述符回收。
 *
 * 测试先让父进程占满 NOFILE 个槽位，同时让一个未继承这些映射的子进程
 * 成功 mmap，证明单进程达到上限不会耗尽全局池。随后在满配额状态下 fork，
 * 验证子进程继承全部 VMA、不能继续 mmap、释放一个槽位后可以重新分配，且
 * 子进程的释放不影响父进程。最后再次 mmap，确认退出和 munmap 已归还资源。
 */
void
vma_limit_test(void)
{
  const char * const f = "mmap.limit";
  char *mappings[NOFILE];
  int fd;
  int pid;
  int status;

  printf("vma_limit_test starting\n");
  testname = "vma_limit_test";
  makefile(f);
  if((fd = open(f, O_RDONLY)) < 0)
    err("open quota file");

  // 子进程在父进程填满配额前创建，因此它自己的 VMA 槽位仍为空。
  int sync_pipe[2];
  if(pipe(sync_pipe) < 0)
    err("pipe");
  if((pid = fork()) < 0)
    err("fork isolation child");
  if(pid == 0){
    char token;
    close(sync_pipe[1]);
    if(read(sync_pipe[0], &token, 1) != 1)
      err("read isolation signal");

    char *child_mapping = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    if(child_mapping == MAP_FAILED)
      err("other process mmap affected by parent quota");
    if(child_mapping[0] != 'A')
      err("other process mapping content");
    if(munmap(child_mapping, PGSIZE) < 0)
      err("other process munmap");

    close(sync_pipe[0]);
    close(fd);
    exit(0);
  }

  close(sync_pipe[0]);
  map_all_vma_slots(fd, mappings);
  if(mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0) != MAP_FAILED)
    err("mmap should fail after NOFILE VMAs");

  char token = 'x';
  if(write(sync_pipe[1], &token, 1) != 1)
    err("write isolation signal");
  close(sync_pipe[1]);

  status = -1;
  if(wait(&status) != pid || status != 0)
    err("independent process VMA isolation");
  unmap_all_vma_slots(mappings);

  // 满配额 fork 必须仍能复制父进程的 NOFILE 个 VMA；全局池为每个进程
  // 预留等量容量，因此正常状态下不会在复制中途耗尽。
  map_all_vma_slots(fd, mappings);
  if((pid = fork()) < 0)
    err("fork with full VMA quota");
  if(pid == 0){
    if(mappings[0][0] != 'A' || mappings[NOFILE - 1][0] != 'A')
      err("fork inherited VMA content");
    if(mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0) != MAP_FAILED)
      err("child should inherit full VMA quota");

    if(munmap(mappings[0], PGSIZE) < 0)
      err("child release inherited VMA");
    char *replacement = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    if(replacement == MAP_FAILED || replacement[0] != 'A')
      err("child reuse released VMA slot");
    if(munmap(replacement, PGSIZE) < 0)
      err("child release replacement VMA");
    close(fd);
    exit(0);
  }

  status = -1;
  if(wait(&status) != pid || status != 0)
    err("forked VMA quota child");
  if(mappings[0][0] != 'A' || mappings[NOFILE - 1][0] != 'A')
    err("child VMA changes affected parent");
  unmap_all_vma_slots(mappings);

  char *recycled = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  if(recycled == MAP_FAILED || recycled[0] != 'A')
    err("VMA descriptor was not recycled");
  if(munmap(recycled, PGSIZE) < 0)
    err("munmap recycled VMA");

  close(fd);
  unlink(f);
  printf("vma_limit_test OK\n");
}
