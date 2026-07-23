#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/stat.h"
#include "user/user.h"

// Lab9 快速回归仍顺序写到二级间接第二张叶子索引表的第一个数据块。
// 三级间接和 4 GiB 边界改由稀疏写覆盖，不再要求顺序写满整个地址空间。
#define LAB9_BOUNDARY_BLOCKS ((uint64)NDIRECT + NINDIRECT + NINDIRECT + 1)
#define FIRST_SINGLE_OFFSET ((uint64)NDIRECT * BSIZE)
#define FIRST_DOUBLE_OFFSET (((uint64)NDIRECT + NINDIRECT) * BSIZE)
#define FIRST_TRIPLE_OFFSET (((uint64)NDIRECT + NINDIRECT + NDOUBLEINDIRECT) * BSIZE)
#define FOUR_GIB ((uint64)1 << 32)
#define SPARSE_SIZE (FOUR_GIB + 2)

struct sparse_marker {
  uint64 offset;
  uchar value;
};

static struct sparse_marker markers[] = {
  {((uint64)NDIRECT - 1) * BSIZE, 0x11},
  {FIRST_SINGLE_OFFSET, 0x22},
  {FIRST_DOUBLE_OFFSET, 0x33},
  {FIRST_TRIPLE_OFFSET, 0x44},
  {FOUR_GIB - 1, 0x51},
  {FOUR_GIB, 0x62},
  {FOUR_GIB + 1, 0x73},
};

static uint buf[BSIZE / sizeof(uint)];

/** 输出稳定诊断、清理本测试文件并以非零状态退出。 */
static void
fail(char *message)
{
  printf("bigfile: %s\n", message);
  unlink("big.file");
  unlink("sparse.file");
  unlink("reuse.file");
  exit(1);
}

/**
 * 将文件描述符定位到绝对偏移并校验 64 位返回值。
 *
 * @param fd 已打开的普通文件描述符。
 * @param offset 目标非负字节偏移。
 */
static void
seek_set_exact(int fd, uint64 offset)
{
  if(lseek(fd, (int64)offset, SEEK_SET) != (int64)offset)
    fail("SEEK_SET returned wrong offset");
}

/**
 * 在指定逻辑偏移写入一个标记字节。
 *
 * @param fd 可写普通文件描述符。
 * @param marker 偏移与期望字节值。
 */
static void
write_marker(int fd, struct sparse_marker *marker)
{
  seek_set_exact(fd, marker->offset);
  if(write(fd, &marker->value, 1) != 1)
    fail("sparse marker write failed");
}

/**
 * 从指定逻辑偏移读回并校验一个标记字节。
 *
 * @param fd 可读普通文件描述符。
 * @param marker 偏移与期望字节值。
 */
static void
read_marker(int fd, struct sparse_marker *marker)
{
  uchar value = 0;

  seek_set_exact(fd, marker->offset);
  if(read(fd, &value, 1) != 1 || value != marker->value)
    fail("sparse marker read mismatch");
}

/**
 * 校验一段完全未映射的 hole 按零返回。
 *
 * @param fd 可读普通文件描述符。
 * @param offset hole 起始字节偏移。
 * @param count 校验字节数，不得超过 buf 大小。
 */
static void
read_zero_hole(int fd, uint64 offset, int count)
{
  uchar *bytes = (uchar*)buf;

  if(count < 0 || (uint)count > (uint)sizeof(buf))
    fail("hole probe too large");
  memset(buf, 0x7f, sizeof(buf));
  seek_set_exact(fd, offset);
  if(read(fd, buf, count) != count)
    fail("hole read failed");
  for(int i = 0; i < count; i++)
    if(bytes[i] != 0)
      fail("hole did not read as zero");
}

/** 顺序覆盖 direct、single-indirect 和 double-indirect 快速边界。 */
static void
test_dense_boundaries(void)
{
  unlink("big.file");
  int fd = open("big.file", O_CREATE | O_RDWR);
  if(fd < 0)
    fail("cannot create dense file");

  for(uint64 block = 0; block < LAB9_BOUNDARY_BLOCKS; block++){
    buf[0] = block;
    if(write(fd, buf, sizeof(buf)) != sizeof(buf))
      fail("dense short write");
  }

  struct stat st;
  if(fstat(fd, &st) < 0)
    fail("dense fstat failed");
  if(st.size != LAB9_BOUNDARY_BLOCKS * BSIZE)
    fail("dense stat size mismatch");
  close(fd);

  fd = open("big.file", O_RDONLY);
  if(fd < 0)
    fail("cannot reopen dense file");
  for(uint64 block = 0; block < LAB9_BOUNDARY_BLOCKS; block++){
    if(read(fd, buf, sizeof(buf)) != sizeof(buf))
      fail("dense short read");
    if(buf[0] != (uint)block)
      fail("dense data mismatch");
  }
  close(fd);

  if(unlink("big.file") < 0)
    fail("dense unlink failed");
}

/**
 * 通过真实 lseek/write/read/fstat/O_TRUNC/unlink 路径验证稀疏文件边界。
 *
 * 测试只为每个目标位置分配一个数据块及必要索引路径，覆盖 direct、single、
 * double、triple 与 2^32-1/2^32/2^32+1，不执行真实 4 GiB 顺序 I/O。
 */
static void
test_sparse_boundaries(void)
{
  struct stat st;
  int fd;
  int pipefd[2];

  unlink("sparse.file");
  fd = open("sparse.file", O_CREATE | O_RDWR);
  if(fd < 0)
    fail("cannot create sparse file");

  // seek 本身只改变打开文件偏移，不得扩大 inode size 或分配数据块。
  seek_set_exact(fd, FOUR_GIB);
  if(fstat(fd, &st) < 0 || st.size != 0)
    fail("seek changed empty file size");

  if(lseek(fd, -1, SEEK_SET) != -1)
    fail("negative SEEK_SET accepted");
  if(lseek(fd, 0, 99) != -1)
    fail("invalid whence accepted");
  if(lseek(fd, (int64)MAXFILE_BYTES + 1, SEEK_SET) != -1)
    fail("seek beyond MAXFILE_BYTES accepted");
  if(lseek(fd, (int64)MAXFILE_BYTES, SEEK_SET) != (int64)MAXFILE_BYTES)
    fail("seek to MAXFILE_BYTES failed");
  uchar sentinel = 0x7a;
  if(write(fd, &sentinel, 1) != -1)
    fail("write beyond MAXFILE_BYTES accepted");
  if(fstat(fd, &st) < 0 || st.size != 0)
    fail("failed max write changed file size");

  for(uint i = 0; i < sizeof(markers) / sizeof(markers[0]); i++)
    write_marker(fd, &markers[i]);

  if(fstat(fd, &st) < 0 || st.size != SPARSE_SIZE)
    fail("sparse stat size mismatch");

  // 整个未映射数据块应返回零；跨 4 GiB 边界读取还应保留相邻标记。
  read_zero_hole(fd, FIRST_TRIPLE_OFFSET + BSIZE, 32);
  seek_set_exact(fd, FOUR_GIB - 2);
  uchar boundary[4] = {0xff, 0xff, 0xff, 0xff};
  if(read(fd, boundary, sizeof(boundary)) != sizeof(boundary))
    fail("4 GiB boundary read failed");
  if(boundary[0] != 0 || boundary[1] != 0x51 ||
     boundary[2] != 0x62 || boundary[3] != 0x73)
    fail("4 GiB boundary bytes mismatch");

  seek_set_exact(fd, FOUR_GIB);
  if(lseek(fd, -1, SEEK_CUR) != (int64)(FOUR_GIB - 1))
    fail("SEEK_CUR failed");
  read_marker(fd, &markers[4]);
  if(lseek(fd, -2, SEEK_END) != (int64)FOUR_GIB)
    fail("SEEK_END failed");
  uchar tail[2];
  if(read(fd, tail, sizeof(tail)) != sizeof(tail) ||
     tail[0] != 0x62 || tail[1] != 0x73)
    fail("SEEK_END tail mismatch");
  if(lseek(fd, -(int64)SPARSE_SIZE - 1, SEEK_END) != -1)
    fail("negative resulting offset accepted");

  // dup() 共享 struct file，因此通过副本读取也必须推进原描述符的同一偏移。
  int shared = dup(fd);
  if(shared < 0)
    fail("dup failed");
  seek_set_exact(fd, FIRST_SINGLE_OFFSET);
  uchar shared_value = 0;
  if(read(shared, &shared_value, 1) != 1 || shared_value != 0x22)
    fail("dup shared offset read failed");
  if(lseek(fd, 0, SEEK_CUR) != (int64)(FIRST_SINGLE_OFFSET + 1))
    fail("dup did not share file offset");
  close(shared);

  if(pipe(pipefd) < 0)
    fail("pipe creation failed");
  if(lseek(pipefd[0], 0, SEEK_SET) != -1)
    fail("pipe unexpectedly seekable");
  close(pipefd[0]);
  close(pipefd[1]);

  int dirfd = open(".", O_RDONLY);
  if(dirfd < 0)
    fail("directory open failed");
  if(lseek(dirfd, 0, SEEK_SET) != -1)
    fail("directory unexpectedly seekable");
  close(dirfd);
  close(fd);

  fd = open("sparse.file", O_RDONLY);
  if(fd < 0)
    fail("cannot reopen sparse file");
  for(uint i = 0; i < sizeof(markers) / sizeof(markers[0]); i++)
    read_marker(fd, &markers[i]);
  read_zero_hole(fd, FIRST_DOUBLE_OFFSET + BSIZE, 32);
  close(fd);

  // O_TRUNC 必须释放所有 direct/indirect 数据与索引块，并把 size 恢复为零。
  fd = open("sparse.file", O_RDWR | O_TRUNC);
  if(fd < 0 || fstat(fd, &st) < 0 || st.size != 0)
    fail("O_TRUNC did not clear sparse file");
  write_marker(fd, &markers[3]);
  write_marker(fd, &markers[6]);
  if(fstat(fd, &st) < 0 || st.size != SPARSE_SIZE)
    fail("sparse rewrite after truncate failed");
  close(fd);
  if(unlink("sparse.file") < 0)
    fail("sparse unlink failed");

  // 删除后再次建立相同 triple/4 GiB 路径，确认释放路径可被后续文件复用。
  fd = open("reuse.file", O_CREATE | O_RDWR);
  if(fd < 0)
    fail("cannot create reuse file");
  write_marker(fd, &markers[3]);
  write_marker(fd, &markers[6]);
  if(fstat(fd, &st) < 0 || st.size != SPARSE_SIZE)
    fail("reclaimed blocks were not reusable");
  close(fd);
  if(unlink("reuse.file") < 0)
    fail("reuse unlink failed");
}

/** 运行 Lab9 快速密集边界与 lseek 稀疏文件专项回归。 */
int
main(void)
{
  test_dense_boundaries();
  test_sparse_boundaries();
  printf("bigfile: verified dense double-indirect and sparse 4 GiB boundaries\n");
  exit(0);
}
