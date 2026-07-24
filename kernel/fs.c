// File system implementation. Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes).
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

typedef char dinode_must_remain_64_bytes[(sizeof(struct dinode) == 64) ? 1 : -1];

// There should be one superblock per disk device, but xv6 uses one device.
struct superblock sb;

// Protects the allocation cursor only. Bitmap contents remain synchronized by
// the buffer cache sleeplock for each bitmap block.
static struct spinlock balloc_lock;
static uint balloc_cursor;

/**
 * Read the file-system superblock from device dev.
 *
 * @param dev Disk device number.
 * @param out Destination superblock.
 */
static void
readsb(int dev, struct superblock *out)
{
  struct buf *bp = bread(dev, 1);
  memmove(out, bp->data, sizeof(*out));
  brelse(bp);
}

/**
 * Initialize the file system and recover its write-ahead log.
 *
 * @param dev Root file-system device.
 */
void
fsinit(int dev)
{
  readsb(dev, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");

  initlock(&balloc_lock, "balloc");
  balloc_cursor = sb.size - sb.nblocks;
  initlog(dev, &sb);
}

/**
 * Zero an allocated block inside the caller's log transaction.
 *
 * @param dev Disk device number.
 * @param bno Block number to clear.
 */
static void
bzero(int dev, int bno)
{
  struct buf *bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

/**
 * Search one half-open data-block range for a free bitmap entry.
 *
 * @param dev Disk device number.
 * @param start First candidate block.
 * @param end One-past-last candidate block.
 * @return Allocated block number, or zero when the range is full.
 */
static uint
balloc_range(uint dev, uint start, uint end)
{
  for(uint b = start; b < end;){
    uint bitmap_base = b - b % BPB;
    struct buf *bp = bread(dev, BBLOCK(b, sb));
    uint first_bit = b - bitmap_base;

    for(uint bi = first_bit; bi < BPB && bitmap_base + bi < end; bi++){
      int mask = 1 << (bi % 8);
      if((bp->data[bi / 8] & mask) == 0){
        uint allocated = bitmap_base + bi;
        bp->data[bi / 8] |= mask;
        log_write(bp);
        brelse(bp);
        bzero(dev, allocated);
        return allocated;
      }
    }

    brelse(bp);
    b = bitmap_base + BPB;
  }
  return 0;
}

/**
 * Allocate and zero one disk block.
 *
 * A short-lived cursor keeps sequential allocation linear instead of rescanning
 * all preceding bitmap bits for every block. Concurrent callers may start from
 * the same hint, but the bitmap buffer sleeplock still makes the allocation
 * decision atomic. Disk exhaustion returns zero instead of panicking.
 *
 * @param dev Disk device number.
 * @return Allocated block number, or zero when the device is full.
 */
static uint
balloc(uint dev)
{
  uint data_start = sb.size - sb.nblocks;

  acquire(&balloc_lock);
  uint start = balloc_cursor;
  release(&balloc_lock);

  uint allocated = balloc_range(dev, start, sb.size);
  if(allocated == 0 && start > data_start)
    allocated = balloc_range(dev, data_start, start);
  if(allocated == 0)
    return 0;

  acquire(&balloc_lock);
  balloc_cursor = allocated + 1;
  if(balloc_cursor >= sb.size)
    balloc_cursor = data_start;
  release(&balloc_lock);
  return allocated;
}

/**
 * Return one allocated block to the free bitmap.
 *
 * @param dev Disk device number.
 * @param b Allocated block number.
 */
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// The icache spinlock protects cache identity and reference counts. Each inode
// sleeplock protects the on-disk fields mirrored below ref/dev/inum.
struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

/** Initialize inode-cache locks. */
void
iinit(void)
{
  initlock(&icache.lock, "icache");
  for(int i = 0; i < NINODE; i++)
    initsleeplock(&icache.inode[i].lock, "inode");
}

static struct inode* iget(uint dev, uint inum);

/**
 * Allocate an on-disk inode and return an unlocked cache reference.
 *
 * @param dev Disk device number.
 * @param type Initial inode type.
 * @return Referenced inode; panics only when the fixed inode table is full.
 */
struct inode*
ialloc(uint dev, short type)
{
  for(int inum = 1; inum < sb.ninodes; inum++){
    struct buf *bp = bread(dev, IBLOCK(inum, sb));
    struct dinode *dip = (struct dinode*)bp->data + inum % IPB;
    if(dip->type == 0){
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

/**
 * Write all persistent fields of a locked in-memory inode to disk.
 *
 * @param ip Locked inode to persist.
 */
void
iupdate(struct inode *ip)
{
  struct buf *bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  struct dinode *dip = (struct dinode*)bp->data + ip->inum % IPB;

  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

/** Find or create an inode-cache reference without locking the inode. */
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty = 0;

  acquire(&icache.lock);
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)
      empty = ip;
  }
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);
  return ip;
}

/** Increment an inode reference count. */
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

/**
 * Lock an inode and lazily populate its persistent fields.
 *
 * @param ip Referenced inode to lock.
 */
void
ilock(struct inode *ip)
{
  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);
  if(ip->valid == 0){
    struct buf *bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    struct dinode *dip = (struct dinode*)bp->data + ip->inum % IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

/** Unlock a referenced inode. */
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");
  releasesleep(&ip->lock);
}

/**
 * Drop an inode reference and reclaim an unlinked final inode.
 *
 * The caller must be inside a log transaction. A large inode can touch hundreds
 * of bitmap blocks while truncating; log.c therefore supports a multi-block
 * commit header and param.h provides enough pinned buffers for the transaction.
 *
 * @param ip Referenced inode to release.
 */
void
iput(struct inode *ip)
{
  acquire(&icache.lock);
  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    acquiresleep(&ip->lock);
    release(&icache.lock);

    itrunc(ip);
    ip->type = 0;
    iupdate(ip);
    ip->valid = 0;

    releasesleep(&ip->lock);
    acquire(&icache.lock);
  }
  ip->ref--;
  release(&icache.lock);
}

/** Unlock an inode and drop the caller's reference. */
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

static const uint64 indirect_capacity[NINDIRECT_LEVELS] = {
  NINDIRECT,
  NDOUBLEINDIRECT,
  NTRIPLEINDIRECT,
};

/**
 * Walk one indirect tree and optionally allocate missing nodes.
 *
 * @param ip Locked inode that owns the root.
 * @param addr Root index-block address.
 * @param bn Block number relative to this tree.
 * @param depth Tree depth: 1, 2, or 3.
 * @param alloc Non-zero to allocate missing index/data blocks.
 * @return Data block address, or zero for a missing block/allocation failure.
 */
static uint
bmap_indirect(struct inode *ip, uint addr, uint64 bn, int depth, int alloc)
{
  uint64 divisor = 1;

  for(int level = 1; level < depth; level++)
    divisor *= NINDIRECT;

  for(int level = depth; level > 0; level--){
    if(addr == 0)
      return 0;

    struct buf *bp = bread(ip->dev, addr);
    uint *entries = (uint*)bp->data;
    uint index = bn / divisor;
    bn %= divisor;

    if(entries[index] == 0){
      if(!alloc){
        brelse(bp);
        return 0;
      }
      uint allocated = balloc(ip->dev);
      if(allocated == 0){
        brelse(bp);
        return 0;
      }
      entries[index] = allocated;
      log_write(bp);
    }

    addr = entries[index];
    brelse(bp);
    if(level > 1)
      divisor /= NINDIRECT;
  }
  return addr;
}

/**
 * Resolve an inode-relative data-block number.
 *
 * @param ip Locked inode.
 * @param bn Zero-based data-block number in the file.
 * @param alloc Non-zero to allocate missing blocks.
 * @return Disk block number, or zero when absent/full/out of range.
 */
static uint
bmap(struct inode *ip, uint64 bn, int alloc)
{
  if(bn < NDIRECT){
    if(ip->addrs[bn] == 0 && alloc)
      ip->addrs[bn] = balloc(ip->dev);
    return ip->addrs[bn];
  }

  uint64 indirect_bn = bn - NDIRECT;
  for(int depth = 1; depth <= NINDIRECT_LEVELS; depth++){
    uint64 capacity = indirect_capacity[depth - 1];
    if(indirect_bn < capacity){
      uint root_index = NDIRECT + depth - 1;
      if(ip->addrs[root_index] == 0){
        if(!alloc)
          return 0;
        ip->addrs[root_index] = balloc(ip->dev);
        if(ip->addrs[root_index] == 0)
          return 0;
      }
      return bmap_indirect(ip, ip->addrs[root_index], indirect_bn,
                           depth, alloc);
    }
    indirect_bn -= capacity;
  }
  return 0;
}

struct indirect_frame {
  uint addr;
  uint next;
  struct buf *bp;
};

/**
 * Free an indirect tree in iterative post-order.
 *
 * @param dev Disk device number.
 * @param root Root index block.
 * @param depth Tree depth from 1 through 3.
 */
static void
bfree_indirect(uint dev, uint root, int depth)
{
  struct indirect_frame stack[NINDIRECT_LEVELS];
  int level = 0;

  stack[0].addr = root;
  stack[0].next = 0;
  stack[0].bp = bread(dev, root);

  while(level >= 0){
    struct indirect_frame *frame = &stack[level];
    uint *entries = (uint*)frame->bp->data;

    if(level == depth - 1){
      while(frame->next < NINDIRECT){
        uint addr = entries[frame->next++];
        if(addr)
          bfree(dev, addr);
      }
    } else {
      while(frame->next < NINDIRECT && entries[frame->next] == 0)
        frame->next++;
      if(frame->next < NINDIRECT){
        uint child = entries[frame->next++];
        level++;
        stack[level].addr = child;
        stack[level].next = 0;
        stack[level].bp = bread(dev, child);
        continue;
      }
    }

    brelse(frame->bp);
    bfree(dev, frame->addr);
    level--;
  }
}

/**
 * Truncate a locked inode and free every direct/index/data block.
 *
 * @param ip Locked inode to clear.
 */
void
itrunc(struct inode *ip)
{
  for(int i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  for(int depth = 1; depth <= NINDIRECT_LEVELS; depth++){
    uint root_index = NDIRECT + depth - 1;
    if(ip->addrs[root_index]){
      bfree_indirect(ip->dev, ip->addrs[root_index], depth);
      ip->addrs[root_index] = 0;
    }
  }

  ip->size = 0;
  iupdate(ip);
}

/** Copy persistent inode metadata into a user-visible stat structure. */
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// 缺失数据块代表稀疏文件 hole；该只读零块可被并发 readi() 安全共享。
static char sparse_zero_block[BSIZE];

/**
 * 从锁定 inode 读取字节；缺失映射作为稀疏 hole 返回零且不分配块。
 *
 * @param ip Locked inode.
 * @param user_dst Whether dst is a user virtual address.
 * @param dst Destination address.
 * @param off 64-bit file offset.
 * @param n Maximum bytes to read.
 * @return Bytes copied, zero at EOF, or -1 when the first copyout fails.
 */
int
readi(struct inode *ip, int user_dst, uint64 dst, uint64 off, uint n)
{
  uint tot = 0;

  if(off > ip->size)
    return 0;
  if((uint64)n > ip->size - off)
    n = ip->size - off;

  while(tot < n){
    uint m = min(n - tot, BSIZE - off % BSIZE);
    uint addr = bmap(ip, off / BSIZE, 0);

    if(addr == 0){
      // seek 越过 EOF 后写入只建立目标路径，中间未映射的数据块按零读取。
      if(either_copyout(user_dst, dst, sparse_zero_block, m) == -1)
        break;
    } else {
      struct buf *bp = bread(ip->dev, addr);
      if(either_copyout(user_dst, dst, bp->data + off % BSIZE, m) == -1){
        brelse(bp);
        break;
      }
      brelse(bp);
    }

    tot += m;
    off += m;
    dst += m;
  }

  if(tot == 0 && n != 0)
    return -1;
  return tot;
}

/**
 * Write bytes to a locked inode using a 64-bit file offset.
 *
 * Callers may overwrite existing data, append, or write beyond EOF. A sparse
 * write allocates only the target data blocks and the indirect-index path needed
 * to reach them; untouched logical blocks remain holes and read back as zero.
 * Disk exhaustion returns a short write (or -1 before the first byte) and
 * persists every newly linked index block so later truncation can reclaim it.
 *
 * @param ip Locked inode.
 * @param user_src Whether src is a user virtual address.
 * @param src Source address.
 * @param off 64-bit file offset.
 * @param n Bytes requested.
 * @return Bytes written, or -1 when no byte could be written.
 */
int
writei(struct inode *ip, int user_src, uint64 src, uint64 off, uint n)
{
  uint tot = 0;
  uint64 end = off + (uint64)n;

  if(end < off || end > MAXFILE_BYTES)
    return -1;

  while(tot < n){
    uint addr = bmap(ip, off / BSIZE, 1);
    if(addr == 0)
      break;

    struct buf *bp = bread(ip->dev, addr);
    uint m = min(n - tot, BSIZE - off % BSIZE);
    if(either_copyin(bp->data + off % BSIZE, user_src, src, m) == -1){
      brelse(bp);
      break;
    }
    log_write(bp);
    brelse(bp);

    tot += m;
    off += m;
    src += m;
  }

  if(n > 0){
    if(off > ip->size)
      ip->size = off;
    // bmap() may have linked a new root before a deeper allocation failed.
    // Persisting addrs[] keeps that partial tree reachable and reclaimable.
    iupdate(ip);
  }

  if(tot == 0 && n != 0)
    return -1;
  return tot;
}

// Directories

/** Compare two fixed-width directory names. */
int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

/** Look up a name in a locked directory. */
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }
  return 0;
}

/** Add a directory entry to a locked directory. */
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");
  return 0;
}

// Paths

/** Copy one path element into name and return the remaining path. */
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

/** Resolve a path, optionally stopping at and naming its parent. */
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

/** Resolve a path to an unlocked inode reference. */
struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

/** Resolve a path's parent and copy its final component into name. */
struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
