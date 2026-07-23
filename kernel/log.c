#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

// 物理 redo log。所有数据块和续接头块落盘后，最后写入首头块中的 n，
// 由这一块原子发布事务；安装完成后清零 n。

#define LOG_FIRST_HEADER_ENTRIES ((BSIZE - sizeof(int)) / sizeof(int))
#define LOG_NEXT_HEADER_ENTRIES  (BSIZE / sizeof(int))

struct logheader {
  int n;
  int block[LOGSIZE];
};

struct log {
  struct spinlock lock;
  int start;
  int size;          // 可用数据槽数量，不包含固定头块。
  int header_blocks;
  int outstanding;
  int committing;
  int dev;
  struct logheader lh;
};

struct log log;

static void recover_from_log(void);
static void commit(void);

/** 返回编码最大日志头所需的固定磁盘块数。 */
static int
log_header_block_count(void)
{
  if(LOGSIZE <= LOG_FIRST_HEADER_ENTRIES)
    return 1;
  return 1 + (LOGSIZE - LOG_FIRST_HEADER_ENTRIES +
              LOG_NEXT_HEADER_ENTRIES - 1) / LOG_NEXT_HEADER_ENTRIES;
}

/** 返回编码 n 个 home block 编号实际需要的头块数。 */
static int
header_blocks_for_entries(int n)
{
  if(n <= LOG_FIRST_HEADER_ENTRIES)
    return 1;
  return 1 + (n - LOG_FIRST_HEADER_ENTRIES +
              LOG_NEXT_HEADER_ENTRIES - 1) / LOG_NEXT_HEADER_ENTRIES;
}

/** 返回第一个日志数据镜像块的位置。 */
static int
log_data_start(void)
{
  return log.start + log.header_blocks;
}

/** 初始化日志几何信息并恢复已发布但未安装的事务。 */
void
initlog(int dev, struct superblock *sb)
{
  initlock(&log.lock, "log");
  log.start = sb->logstart;
  log.header_blocks = log_header_block_count();
  log.size = sb->nlog - log.header_blocks;
  log.dev = dev;

  if(log.size < MAXOPBLOCKS || log.size > LOGSIZE)
    panic("initlog: invalid log size");

  recover_from_log();
}

/** 把每个已提交日志镜像复制回 home block。 */
static void
install_trans(int recovering)
{
  for(int tail = 0; tail < log.lh.n; tail++){
    struct buf *lbuf = bread(log.dev, log_data_start() + tail);
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]);
    memmove(dbuf->data, lbuf->data, BSIZE);
    bwrite(dbuf);
    if(!recovering)
      bunpin(dbuf);
    brelse(lbuf);
    brelse(dbuf);
  }
}

/** 从多块日志头恢复已发布的 home block 列表。 */
static void
read_head(void)
{
  struct buf *first = bread(log.dev, log.start);
  int *words = (int*)first->data;
  int n = words[0];

  if(n < 0 || n > log.size || n > LOGSIZE){
    brelse(first);
    panic("read_head: invalid count");
  }

  log.lh.n = n;
  int copied = min(n, LOG_FIRST_HEADER_ENTRIES);
  for(int i = 0; i < copied; i++)
    log.lh.block[i] = words[i + 1];
  brelse(first);

  int needed = header_blocks_for_entries(n);
  for(int header = 1; header < needed; header++){
    struct buf *bp = bread(log.dev, log.start + header);
    int *entries = (int*)bp->data;
    int remaining = n - copied;
    int count = min(remaining, LOG_NEXT_HEADER_ENTRIES);
    for(int i = 0; i < count; i++)
      log.lh.block[copied + i] = entries[i];
    copied += count;
    brelse(bp);
  }
}

/**
 * 持久化内存日志头。
 *
 * 先写续接头块，最后写含 n 的首头块，使单块写入仍是事务发布点。
 */
static void
write_head(void)
{
  int n = log.lh.n;

  if(n == 0){
    struct buf *bp = bread(log.dev, log.start);
    memset(bp->data, 0, BSIZE);
    bwrite(bp);
    brelse(bp);
    return;
  }

  int needed = header_blocks_for_entries(n);
  int copied = LOG_FIRST_HEADER_ENTRIES;
  for(int header = 1; header < needed; header++){
    struct buf *bp = bread(log.dev, log.start + header);
    memset(bp->data, 0, BSIZE);
    int *entries = (int*)bp->data;
    int remaining = n - copied;
    int count = min(remaining, LOG_NEXT_HEADER_ENTRIES);
    for(int i = 0; i < count; i++)
      entries[i] = log.lh.block[copied + i];
    copied += count;
    bwrite(bp);
    brelse(bp);
  }

  struct buf *first = bread(log.dev, log.start);
  memset(first->data, 0, BSIZE);
  int *words = (int*)first->data;
  int count = min(n, LOG_FIRST_HEADER_ENTRIES);
  for(int i = 0; i < count; i++)
    words[i + 1] = log.lh.block[i];
  words[0] = n;
  bwrite(first);
  brelse(first);
}

/** 重放已发布事务并清除其提交标记。 */
static void
recover_from_log(void)
{
  read_head();
  install_trans(1);
  log.lh.n = 0;
  write_head();
}

/** 在持锁状态判断能否再预留一个最坏文件系统操作。 */
static int
log_has_space_for_new_op_locked(void)
{
  int reserved = log.lh.n + (log.outstanding + 1) * MAXOPBLOCKS;
  return reserved <= log.size;
}

/** 进入文件系统操作；提交中或空间不足时睡眠。 */
void
begin_op(void)
{
  acquire(&log.lock);
  while(log.committing || !log_has_space_for_new_op_locked())
    sleep(&log, &log.lock);
  log.outstanding++;
  release(&log.lock);
}

/** 离开文件系统操作；最后一个参与者负责提交。 */
void
end_op(void)
{
  acquire(&log.lock);
  log.outstanding--;
  if(log.committing)
    panic("log.committing");

  if(log.outstanding != 0){
    wakeup(&log);
    release(&log.lock);
    return;
  }

  log.committing = 1;
  release(&log.lock);
  commit();
  acquire(&log.lock);
  log.committing = 0;
  wakeup(&log);
  release(&log.lock);
}

/** 把修改后的 home buffer 复制到连续日志数据槽。 */
static void
write_log(void)
{
  for(int tail = 0; tail < log.lh.n; tail++){
    struct buf *to = bread(log.dev, log_data_start() + tail);
    struct buf *from = bread(log.dev, log.lh.block[tail]);
    memmove(to->data, from->data, BSIZE);
    bwrite(to);
    brelse(from);
    brelse(to);
  }
}

/** 提交当前吸收后的事务。 */
static void
commit(void)
{
  if(log.lh.n > 0){
    write_log();
    write_head();
    install_trans(0);
    log.lh.n = 0;
    write_head();
  }
}

/**
 * 将一个已锁定的修改 buffer 吸收到当前事务，并保持 pin 直到提交。
 *
 * @param b 当前内容必须进入日志的已锁定 buffer。
 */
void
log_write(struct buf *b)
{
  if(log.outstanding < 1)
    panic("log_write outside of trans");

  acquire(&log.lock);
  int i;
  for(i = 0; i < log.lh.n; i++)
    if(log.lh.block[i] == b->blockno)
      break;

  if(i == log.lh.n){
    if(log.lh.n >= log.size || log.lh.n >= LOGSIZE){
      release(&log.lock);
      panic("too big a transaction");
    }
    log.lh.block[i] = b->blockno;
    bpin(b);
    log.lh.n++;
  }
  release(&log.lock);
}
