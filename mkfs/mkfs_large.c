#include <sys/types.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define stat xv6_stat  // avoid clash with the host struct stat tag
#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/param.h"

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

#define NINODES 200

int nbitmap = FSSIZE / (BSIZE * 8) + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGSIZE;
int nmeta;
int nblocks;

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;

void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint, struct dinode*);
void rsect(uint, void*);
uint ialloc(ushort);
void iappend(uint, void*, int);
void die(const char*);

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

uint64
xlong(uint64 x)
{
  uint64 y;
  uchar *a = (uchar*)&y;
  for(int i = 0; i < 8; i++)
    a[i] = x >> (8 * i);
  return y;
}

ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

/** Exit mkfs after printing one host-side failure. */
void
die(const char *message)
{
  perror(message);
  exit(1);
}

/** Write one complete file-system sector using an off_t-safe offset. */
void
wsect(uint sec, void *buf)
{
  off_t offset = (off_t)sec * BSIZE;
  if(lseek(fsfd, offset, SEEK_SET) != offset)
    die("lseek write");
  if(write(fsfd, buf, BSIZE) != BSIZE)
    die("write sector");
}

/** Read one complete file-system sector using an off_t-safe offset. */
void
rsect(uint sec, void *buf)
{
  off_t offset = (off_t)sec * BSIZE;
  if(lseek(fsfd, offset, SEEK_SET) != offset)
    die("lseek read");
  if(read(fsfd, buf, BSIZE) != BSIZE)
    die("read sector");
}

void
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  struct dinode *dip = ((struct dinode*)buf) + inum % IPB;
  *dip = *ip;
  wsect(bn, buf);
}

void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  struct dinode *dip = ((struct dinode*)buf) + inum % IPB;
  *ip = *dip;
}

uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;
  memset(&din, 0, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xlong(0);
  winode(inum, &din);
  return inum;
}

/**
 * Append host bytes to one image inode.
 *
 * Initial image inputs are small, so mkfs only needs direct and single-indirect
 * construction. The running kernel owns double/triple-indirect growth.
 */
void
iappend(uint inum, void *xp, int n)
{
  char *p = xp;
  uint64 off;
  struct dinode din;
  char buf[BSIZE];
  uint indirect[NINDIRECT];

  rinode(inum, &din);
  off = xlong(din.size);

  while(n > 0){
    uint64 fbn = off / BSIZE;
    assert(fbn < (uint64)NDIRECT + NINDIRECT);
    uint x;

    if(fbn < NDIRECT){
      if(xint(din.addrs[fbn]) == 0)
        din.addrs[fbn] = xint(freeblock++);
      x = xint(din.addrs[fbn]);
    } else {
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(freeblock++);
        memset(indirect, 0, sizeof(indirect));
        wsect(xint(din.addrs[NDIRECT]), indirect);
      }
      rsect(xint(din.addrs[NDIRECT]), indirect);
      if(indirect[fbn - NDIRECT] == 0){
        indirect[fbn - NDIRECT] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT]), indirect);
      }
      x = xint(indirect[fbn - NDIRECT]);
    }

    int n1 = n;
    if(n1 > BSIZE - off % BSIZE)
      n1 = BSIZE - off % BSIZE;
    rsect(x, buf);
    memmove(buf + off % BSIZE, p, n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }

  din.size = xlong(off);
  winode(inum, &din);
}

/** Mark every block in the allocated prefix across all bitmap sectors. */
void
balloc(int used)
{
  uchar buf[BSIZE];

  for(int bitmap = 0; bitmap < nbitmap; bitmap++){
    memset(buf, 0, sizeof(buf));
    int start = bitmap * BPB;
    int count = used - start;
    if(count < 0)
      count = 0;
    if(count > BPB)
      count = BPB;
    for(int bit = 0; bit < count; bit++)
      buf[bit / 8] |= 1 << (bit % 8);
    wsect(xint(sb.bmapstart) + bitmap, buf);
  }
}

int
main(int argc, char *argv[])
{
  int i, cc, fd;
  uint rootino, inum, off;
  struct dirent de;
  char buf[BSIZE];
  struct dinode din;

  static_assert(sizeof(int) == 4, "integers must be 4 bytes");
  static_assert(sizeof(uint64) == 8, "uint64 must be 8 bytes");
  static_assert(sizeof(struct dinode) == 64, "dinode must remain 64 bytes");
  static_assert(BSIZE % sizeof(struct dinode) == 0, "integral inodes per block");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs-large fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dirent)) == 0);
  nmeta = 2 + nlog + ninodeblocks + nbitmap;
  nblocks = FSSIZE - nmeta;

  fsfd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0666);
  if(fsfd < 0)
    die("open image");
  if(ftruncate(fsfd, (off_t)FSSIZE * BSIZE) < 0)
    die("ftruncate image");

  sb.magic = FSMAGIC;
  sb.size = xint(FSSIZE);
  sb.nblocks = xint(nblocks);
  sb.ninodes = xint(NINODES);
  sb.nlog = xint(nlog);
  sb.logstart = xint(2);
  sb.inodestart = xint(2 + nlog);
  sb.bmapstart = xint(2 + nlog + ninodeblocks);

  printf("nmeta %d (boot, super, log blocks %u inode blocks %u, bitmap blocks %u) blocks %d total %d\n",
         nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  freeblock = nmeta;
  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  memset(&de, 0, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

  for(i = 2; i < argc; i++){
    char *shortname = argv[i];
    if(strncmp(shortname, "user/", 5) == 0)
      shortname += 5;
    assert(index(shortname, '/') == 0);

    if((fd = open(argv[i], O_RDONLY)) < 0)
      die(argv[i]);
    if(shortname[0] == '_')
      shortname++;

    inum = ialloc(T_FILE);
    memset(&de, 0, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, shortname, DIRSIZ);
    iappend(rootino, &de, sizeof(de));

    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);
    close(fd);
  }

  rinode(rootino, &din);
  off = xlong(din.size);
  off = ((off / BSIZE) + 1) * BSIZE;
  din.size = xlong(off);
  winode(rootino, &din);

  balloc(freeblock);
  close(fsfd);
  return 0;
}
