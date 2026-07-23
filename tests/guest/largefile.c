#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/stat.h"
#include "user/user.h"

#define BLOCKS_PER_IO 256
#define TARGET_BLOCKS (((uint64)1 << 32) / BSIZE + 1)
#define TARGET_BYTES (TARGET_BLOCKS * (uint64)BSIZE)
#define REUSE_BLOCKS ((uint64)NDIRECT + NINDIRECT + NDOUBLEINDIRECT + 1)
#define PROGRESS_BLOCKS (256 * 1024)

static uint64 io_words[(BLOCKS_PER_IO * BSIZE) / sizeof(uint64)];

/** Print a stable failure, remove test files, and exit non-zero. */
static void
fail(char *message)
{
  printf("largefile: FAIL %s\n", message);
  unlink("large.file");
  unlink("fill.file");
  unlink("reuse.file");
  exit(1);
}

/** Fill count complete blocks with a deterministic block/word pattern. */
static void
fill_pattern(uint64 first_block, int count)
{
  int words_per_block = BSIZE / sizeof(uint64);
  for(int block = 0; block < count; block++){
    uint64 logical = first_block + block;
    for(int word = 0; word < words_per_block; word++)
      io_words[block * words_per_block + word] = logical ^ ((uint64)word << 32);
  }
}

/** Verify count complete blocks against fill_pattern(). */
static int
verify_pattern(uint64 first_block, int count)
{
  int words_per_block = BSIZE / sizeof(uint64);
  for(int block = 0; block < count; block++){
    uint64 logical = first_block + block;
    for(int word = 0; word < words_per_block; word++){
      uint64 expected = logical ^ ((uint64)word << 32);
      if(io_words[block * words_per_block + word] != expected)
        return -1;
    }
  }
  return 0;
}

/** Sequentially write an exact number of patterned blocks. */
static void
write_exact_blocks(int fd, uint64 blocks, char *phase)
{
  uint64 block = 0;
  while(block < blocks){
    int count = BLOCKS_PER_IO;
    if(blocks - block < (uint64)count)
      count = blocks - block;
    fill_pattern(block, count);
    int bytes = count * BSIZE;
    if(write(fd, io_words, bytes) != bytes)
      fail(phase);
    block += count;
    if(block % PROGRESS_BLOCKS == 0 || block == blocks)
      printf("largefile: %s blocks=%l\n", phase, block);
  }
}

/** Sequentially read and verify an exact number of patterned blocks. */
static void
read_exact_blocks(int fd, uint64 blocks)
{
  uint64 block = 0;
  while(block < blocks){
    int count = BLOCKS_PER_IO;
    if(blocks - block < (uint64)count)
      count = blocks - block;
    int bytes = count * BSIZE;
    if(read(fd, io_words, bytes) != bytes)
      fail("short read before 4 GiB boundary");
    if(verify_pattern(block, count) < 0)
      fail("data mismatch");
    block += count;
    if(block % PROGRESS_BLOCKS == 0 || block == blocks)
      printf("largefile: read blocks=%l\n", block);
  }
}

/** Consume remaining data blocks and confirm disk-full returns without panic. */
static uint64
fill_until_full(void)
{
  int fd = open("fill.file", O_CREATE | O_RDWR);
  if(fd < 0)
    fail("cannot create fill.file");

  uint64 bytes = 0;
  for(;;){
    fill_pattern(bytes / BSIZE, BLOCKS_PER_IO);
    int requested = BLOCKS_PER_IO * BSIZE;
    int written = write(fd, io_words, requested);
    if(written < 0)
      break;
    bytes += written;
    if(written != requested)
      break;
  }

  struct stat st;
  if(fstat(fd, &st) < 0 || st.size != bytes)
    fail("disk-full stat mismatch");
  close(fd);
  printf("largefile: disk full handled bytes=%l\n", bytes);
  return bytes;
}

int
main(void)
{
  unlink("large.file");
  unlink("fill.file");
  unlink("reuse.file");

  int fd = open("large.file", O_CREATE | O_RDWR);
  if(fd < 0)
    fail("cannot create large.file");
  write_exact_blocks(fd, TARGET_BLOCKS, "write");

  struct stat st;
  if(fstat(fd, &st) < 0)
    fail("target fstat failed");
  if(st.size != TARGET_BYTES)
    fail("target did not cross 2^32 bytes");
  close(fd);

  fd = open("large.file", O_RDONLY);
  if(fd < 0)
    fail("cannot reopen large.file");
  read_exact_blocks(fd, TARGET_BLOCKS);
  close(fd);

  fill_until_full();

  if(unlink("large.file") < 0 || unlink("fill.file") < 0)
    fail("large unlink failed");

  fd = open("reuse.file", O_CREATE | O_RDWR);
  if(fd < 0)
    fail("cannot create reuse.file");
  write_exact_blocks(fd, REUSE_BLOCKS, "reuse");
  if(fstat(fd, &st) < 0 || st.size != REUSE_BLOCKS * BSIZE)
    fail("reclaimed blocks were not reusable");
  close(fd);
  if(unlink("reuse.file") < 0)
    fail("reuse unlink failed");

  printf("largefile: PASS bytes=%l\n", TARGET_BYTES);
  exit(0);
}
