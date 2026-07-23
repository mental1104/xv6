#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/stat.h"
#include "user/user.h"

// Lab9 remains a focused direct/single/double-indirect regression. The
// 4-GiB/triple-indirect path is isolated in largefile.c and is not part of the
// normal PR suite.
#define LAB9_BLOCKS ((uint64)NDIRECT + NINDIRECT + NDOUBLEINDIRECT)

static uint buf[BSIZE / sizeof(uint)];

/** Fail the guest test with a stable diagnostic and non-zero status. */
static void
fail(char *message)
{
  printf("bigfile: %s\n", message);
  unlink("big.file");
  exit(1);
}

int
main(void)
{
  unlink("big.file");
  int fd = open("big.file", O_CREATE | O_RDWR);
  if(fd < 0)
    fail("cannot create file");

  for(uint64 block = 0; block < LAB9_BLOCKS; block++){
    buf[0] = block;
    if(write(fd, buf, sizeof(buf)) != sizeof(buf))
      fail("short write");
  }

  struct stat st;
  if(fstat(fd, &st) < 0)
    fail("fstat failed");
  if(st.size != LAB9_BLOCKS * BSIZE)
    fail("wrong stat size");
  close(fd);

  fd = open("big.file", O_RDONLY);
  if(fd < 0)
    fail("cannot reopen file");
  for(uint64 block = 0; block < LAB9_BLOCKS; block++){
    if(read(fd, buf, sizeof(buf)) != sizeof(buf))
      fail("short read");
    if(buf[0] != (uint)block)
      fail("data mismatch");
  }
  close(fd);

  if(unlink("big.file") < 0)
    fail("unlink failed");
  printf("bigfile: wrote and read %l double-indirect blocks\n", LAB9_BLOCKS);
  exit(0);
}
