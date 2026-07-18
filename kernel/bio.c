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
static struct buf*
find_unused_locked(uint idx)
{
  struct buf *b;
  struct buf *head = &bcache.head[idx];

  for(b = head->prev; b != head; b = b->prev){
    if(b->refcnt == 0)
      return b;
  }

  return 0;
}

// Caller must have exclusive access to b.
static void
prepare_buf(struct buf *b, uint dev, uint blockno)
{
  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
}

// Caller must hold the source and target bucket locks.
static void
move_buf_locked(struct buf *b, uint target_idx)
{
  b->prev->next = b->next;
  b->next->prev = b->prev;

  b->next = bcache.head[target_idx].next;
  b->prev = &bcache.head[target_idx];
  b->next->prev = b;
  b->prev->next = b;
}

// Find the requested block or recycle an unused buffer in one bucket.
// Caller must hold bcache.lock[idx].
static struct buf*
try_get_local_locked(uint idx, uint dev, uint blockno)
{
  struct buf *b;
  struct buf *head = &bcache.head[idx];

  for(b = head->next; b != head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      return b;
    }
  }

  b = find_unused_locked(idx);
  if(b != 0)
    prepare_buf(b, dev, blockno);

  return b;
}

// Recheck the target bucket while cross-bucket moves are serialized.
// If the target bucket still has no usable buffer, steal one from another bucket.
static struct buf*
get_or_steal(uint target_idx, uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.steal_lock);
  acquire(&bcache.lock[target_idx]);

  b = try_get_local_locked(target_idx, dev, blockno);
  if(b != 0)
    goto found;

  for(uint victim_idx = ihash(target_idx + 1);
      victim_idx != target_idx;
      victim_idx = ihash(victim_idx + 1)){
    acquire(&bcache.lock[victim_idx]);

    b = find_unused_locked(victim_idx);
    if(b == 0){
      release(&bcache.lock[victim_idx]);
      continue;
    }

    prepare_buf(b, dev, blockno);
    move_buf_locked(b, target_idx);
    release(&bcache.lock[victim_idx]);
    goto found;
  }

  release(&bcache.lock[target_idx]);
  release(&bcache.steal_lock);
  panic("bget: no buffers");

found:
  release(&bcache.lock[target_idx]);
  release(&bcache.steal_lock);
  return b;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint idx = ihash(blockno);

  acquire(&bcache.lock[idx]);
  b = try_get_local_locked(idx, dev, blockno);
  release(&bcache.lock[idx]);

  if(b == 0)
    b = get_or_steal(idx, dev, blockno);

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
