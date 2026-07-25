#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "fsinspect.h"
#include "buf.h"
#include "file.h"

extern struct superblock sb;

/** 统计磁盘 inode 表中 type 非零的已分配 inode。 */
static uint64
count_allocated_inodes(uint dev)
{
  uint64 count = 0;

  for(uint inum = 1; inum < sb.ninodes; inum++){
    struct buf *bp = bread(dev, IBLOCK(inum, sb));
    struct dinode *dip = (struct dinode*)bp->data + inum % IPB;
    if(dip->type != 0)
      count++;
    brelse(bp);
  }

  return count;
}

/** 统计数据区位图中已经置位的块，不把固定元数据块计入文件数据分配。 */
static uint64
count_allocated_blocks(uint dev)
{
  uint data_start = sb.size - sb.nblocks;
  uint64 count = 0;

  for(uint b = data_start; b < sb.size;){
    uint bitmap_base = b - b % BPB;
    struct buf *bp = bread(dev, BBLOCK(b, sb));
    uint first_bit = b - bitmap_base;

    for(uint bit = first_bit; bit < BPB && bitmap_base + bit < sb.size; bit++)
      if(bp->data[bit / 8] & (1 << (bit % 8)))
        count++;

    brelse(bp);
    b = bitmap_base + BPB;
  }

  return count;
}

/**
 * 读取一级间接根的第一个叶子地址。
 *
 * @param ip 调用者已持有 sleeplock 的 inode。
 * @return 逻辑块 NDIRECT 对应的数据块；根或首项不存在时返回零。
 */
static uint
first_indirect_data_block(struct inode *ip)
{
  uint root = ip->addrs[NDIRECT];
  if(root == 0)
    return 0;

  struct buf *bp = bread(ip->dev, root);
  uint first = ((uint*)bp->data)[0];
  brelse(bp);
  return first;
}

/**
 * 收集文件系统全局状态，并可选地加入一个已锁定 inode 的块映射边界。
 *
 * @param ip 已锁定的 inode；传入 0 时只收集全局状态。
 * @param out 调用者持有的输出快照。
 */
void
fsinspect_collect(struct inode *ip, struct fsinspect_snapshot *out)
{
  memset(out, 0, sizeof(*out));
  out->version = FSINSPECT_VERSION;
  out->total_blocks = sb.size;
  out->data_blocks = sb.nblocks;
  out->data_start = sb.size - sb.nblocks;
  out->inode_count = sb.ninodes;
  out->log_start = sb.logstart;
  out->log_blocks = sb.nlog;
  out->inode_start = sb.inodestart;
  out->bitmap_start = sb.bmapstart;

  // 先取运行时计数，让本次扫描本身产生的缓存访问只进入下一张快照。
  bcache_stats_snapshot(&out->cache);
  log_stats_snapshot(&out->log);

  out->allocated_inodes = count_allocated_inodes(ROOTDEV);
  out->allocated_blocks = count_allocated_blocks(ROOTDEV);

  if(ip == 0)
    return;

  out->has_inode = 1;
  out->inode_number = ip->inum;
  out->inode_type = ip->type;
  out->inode_nlink = ip->nlink;
  out->inode_size = ip->size;
  out->direct_first = ip->addrs[0];
  out->direct_last = ip->addrs[NDIRECT - 1];
  out->indirect_root = ip->addrs[NDIRECT];
  out->indirect_first = first_indirect_data_block(ip);
}
