// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "fsinspect.h"
#include "buf.h"

#define NBUCKET 13

struct {
  struct buf head[NBUCKET];
  struct spinlock lock[NBUCKET];
  struct buf buf[NBUF];
  struct spinlock steal_lock;
} bcache;

struct {
  struct spinlock lock;
  struct fsinspect_cache_stats value;
} bcache_stats;

uint ihash(uint blockno){
  return blockno % NBUCKET;
}

char buf[NBUCKET][20];

/** 记录一次已经完成归类的缓存查找。 */
static void
record_cache_lookup(int hit)
{
  acquire(&bcache_stats.lock);
  bcache_stats.value.requests++;
  if(hit)
    bcache_stats.value.hits++;
  else
    bcache_stats.value.misses++;
  release(&bcache_stats.lock);
}

/** 记录一次从其他哈希桶迁移空闲 buffer 的慢路径。 */
static void
record_cache_steal(void)
{
  acquire(&bcache_stats.lock);
  bcache_stats.value.steals++;
  release(&bcache_stats.lock);
}

/** 记录一次真实设备读取。 */
static void
record_disk_read(void)
{
  acquire(&bcache_stats.lock);
  bcache_stats.value.disk_reads++;
  release(&bcache_stats.lock);
}

/** 记录一次真实设备写入。 */
static void
record_disk_write(void)
{
  acquire(&bcache_stats.lock);
  bcache_stats.value.disk_writes++;
  release(&bcache_stats.lock);
}

/** 把累计 buffer cache 计数复制到调用者持有的快照。 */
void
bcache_stats_snapshot(struct fsinspect_cache_stats *out)
{
  acquire(&bcache_stats.lock);
  *out = bcache_stats.value;
  release(&bcache_stats.lock);
}

void
binit(void)
{
  struct buf *b;

  initlock(&bcache_stats.lock, "bcache.stats");
  memset(&bcache_stats.value, 0, sizeof(bcache_stats.value));

  for(int i = 0; i < NBUCKET; i++) {
    snprintf(buf[i], 20, "bcache.bucket%d", i);//13 BUCKETS  
    initlock(&bcache.lock[i], (char *)buf[i]);
  }
  initlock(&bcache.steal_lock, "bcache");

  for(int i = 0; i < NBUCKET; i++){
    // create a circular linked list
    // head.next is the first elem
    // head.prev is the last elem
    struct buf *head = &bcache.head[i];
    head->prev = head;
    head->next = head;
  }
  int i;

  for (b = bcache.buf, i = 0; b < bcache.buf + NBUF; b++, i = (i + 1) % NBUCKET){
    b->next = bcache.head[i].next;
    b->prev = &bcache.head[i];
    bcache.head[i].next->prev = b;
    bcache.head[i].next = b;
    initsleeplock(&b->lock, "buffer");
  }
  // Create linked list of buffers
}

// Caller must hold bcache.lock[idx].
static struct buf *
find_cached_locked(uint idx, uint dev, uint blockno)
{
  struct buf *head = &bcache.head[idx];

  for(struct buf *b = head->next; b != head; b = b->next){
    if(b->dev == dev && b->blockno == blockno)
      return b;
  }

  return 0;
}

// Caller must hold bcache.lock[idx].
// Search from the least recently used end.
static struct buf *
find_unused_locked(uint idx)
{
  struct buf *head = &bcache.head[idx];

  for(struct buf *b = head->prev; b != head; b = b->prev){
    if(b->refcnt == 0)
      return b;
  }

  return 0;
}

/** 初始化一个准备承载指定磁盘块的空闲 buffer。 */
static void
prepare_buf(struct buf *b, uint dev, uint blockno)
{
  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
}

/** 从当前哈希链摘除一个调用者已锁定所在桶的 buffer。 */
static void
unlink_buf(struct buf *b)
{
  b->prev->next = b->next;
  b->next->prev = b->prev;
}

/** 把 buffer 插入目标桶的最近使用端；调用者持有目标桶锁。 */
static void
insert_buf_front(uint idx, struct buf *b)
{
  struct buf *head = &bcache.head[idx];

  b->next = head->next;
  b->prev = head;
  head->next->prev = b;
  head->next = b;
}

/**
 * 在目标桶中查找已有副本，或复用一个本地空闲 buffer。
 *
 * @param idx 调用者已锁定的目标桶。
 * @param dev 设备号。
 * @param blockno 磁盘块号。
 * @param hit 找到已有副本时写入 1；复用空闲 buffer 时写入 0。
 * @return 已增加引用或重新初始化的 buffer；没有本地候选时返回 0。
 */
static struct buf *
try_get_local_locked(uint idx, uint dev, uint blockno, int *hit)
{
  struct buf *b;

  b = find_cached_locked(idx, dev, blockno);
  if(b != 0){
    b->refcnt++;
    *hit = 1;
    return b;
  }

  b = find_unused_locked(idx);
  if(b != 0){
    prepare_buf(b, dev, blockno);
    *hit = 0;
    return b;
  }

  return 0;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return a locked buffer.
static struct buf *
bget(uint dev, uint blockno)
{
  uint home_idx = ihash(blockno);
  int holding_steal_lock = 0;
  int cache_hit = 0;
  struct buf *b;

  /*
   * Fast path:
   * Search or recycle a buffer in the target bucket.
   */
  acquire(&bcache.lock[home_idx]);

  b = try_get_local_locked(home_idx, dev, blockno, &cache_hit);
  if(b != 0){
    record_cache_lookup(cache_hit);
    goto found;
  }

  release(&bcache.lock[home_idx]);

  /*
   * Slow path:
   * Serialize cross-bucket stealing.
   */
  acquire(&bcache.steal_lock);
  holding_steal_lock = 1;

  acquire(&bcache.lock[home_idx]);

  /*
   * The target bucket may have changed while its lock was released.
   * Recheck it to avoid creating duplicate buffers for one disk block.
   */
  b = try_get_local_locked(home_idx, dev, blockno, &cache_hit);
  if(b != 0){
    record_cache_lookup(cache_hit);
    goto found;
  }

  /*
   * Keep the target bucket locked while moving a buffer into it.
   */
  for(uint victim_idx = ihash(home_idx + 1);
      victim_idx != home_idx;
      victim_idx = ihash(victim_idx + 1)){

    acquire(&bcache.lock[victim_idx]);

    b = find_unused_locked(victim_idx);
    if(b == 0){
      release(&bcache.lock[victim_idx]);
      continue;
    }

    prepare_buf(b, dev, blockno);
    unlink_buf(b);

    release(&bcache.lock[victim_idx]);

    insert_buf_front(home_idx, b);
    record_cache_lookup(0);
    record_cache_steal();
    goto found;
  }

  release(&bcache.lock[home_idx]);
  release(&bcache.steal_lock);

  panic("bget: no buffers");

found:
  release(&bcache.lock[home_idx]);

  if(holding_steal_lock)
    release(&bcache.steal_lock);

  /*
   * Never wait on a sleeplock while holding a spinlock.
   */
  acquiresleep(&b->lock);
  return b;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
    record_disk_read();
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
  record_disk_write();
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint idx = ihash(b->blockno);
  acquire(&bcache.lock[idx]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head[idx].next;
    b->prev = &bcache.head[idx];
    bcache.head[idx].next->prev = b;
    bcache.head[idx].next = b;
  }
  
  release(&bcache.lock[idx]);
}

void
bpin(struct buf *b) {
  uint idx = ihash(b->blockno);
  acquire(&bcache.lock[idx]);
  b->refcnt++;
  release(&bcache.lock[idx]);
}

void
bunpin(struct buf *b) {
  uint idx = ihash(b->blockno);
  acquire(&bcache.lock[idx]);
  b->refcnt--;
  release(&bcache.lock[idx]);
}
