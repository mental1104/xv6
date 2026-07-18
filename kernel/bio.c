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
#include "buf.h"

#define NBUCKET 13

struct {
  struct buf head[NBUCKET];
  struct spinlock lock[NBUCKET];
  struct buf buf[NBUF];
  struct spinlock steal_lock;
} bcache;

uint ihash(uint blockno){
  return blockno % NBUCKET;
}

char buf[NBUCKET][20];

void
binit(void)
{
  struct buf *b;

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

static void
prepare_buf(struct buf *b, uint dev, uint blockno)
{
  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
}

static void
unlink_buf(struct buf *b)
{
  b->prev->next = b->next;
  b->next->prev = b->prev;
}

static void
insert_buf_front(uint idx, struct buf *b)
{
  struct buf *head = &bcache.head[idx];

  b->next = head->next;
  b->prev = head;
  head->next->prev = b;
  head->next = b;
}

// Try to find the requested block or recycle an unused local buffer.
// Caller must hold bcache.lock[idx].
static struct buf *
try_get_local_locked(uint idx, uint dev, uint blockno)
{
  struct buf *b;

  b = find_cached_locked(idx, dev, blockno);
  if(b != 0){
    b->refcnt++;
    return b;
  }

  b = find_unused_locked(idx);
  if(b != 0){
    prepare_buf(b, dev, blockno);
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
  struct buf *b;

  /*
   * Fast path:
   * Search or recycle a buffer in the target bucket.
   */
  acquire(&bcache.lock[home_idx]);

  b = try_get_local_locked(home_idx, dev, blockno);
  if(b != 0)
    goto found;

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
  b = try_get_local_locked(home_idx, dev, blockno);
  if(b != 0)
    goto found;

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


