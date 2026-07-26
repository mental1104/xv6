//
// driver for qemu's virtio disk device.
// uses qemu's mmio interface to virtio.
// qemu presents a "legacy" virtio interface.
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"
#include "disktrace_abi.h"

// the address of virtio mmio register r.
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))

static struct disk {
  // memory for virtio descriptors &c for queue 0.
  // this is a global instead of allocated because it must
  // be multiple contiguous pages, which kalloc()
  // doesn't support, and page aligned.
  char pages[2*PGSIZE];
  struct VRingDesc *desc;
  uint16 *avail;
  struct UsedArea *used;

  // our own book-keeping.
  char free[NUM];  // is a descriptor free?
  uint16 used_idx; // we've looked this far in used[2..NUM].

  // track info about in-flight operations,
  // for use when completion interrupt arrives.
  // indexed by first descriptor index of chain.
  struct {
    struct buf *b;
    char status;
    uint64 request_id;
    uint64 blockno;
    int write;
  } info[NUM];

  // 轨迹状态与 virtqueue 元数据共用 vdisk_lock，保证事件顺序与驱动状态一致。
  int trace_active;
  int trace_dropped;
  int trace_events;
  uint64 trace_next_seq;
  uint64 next_request_id;
  struct disktrace_event trace_events_buffer[DISKTRACE_MAX_EVENTS];

  struct spinlock vdisk_lock;

} __attribute__ ((aligned (PGSIZE))) disk;

/**
 * append_trace_event_locked 追加一个驱动可见的请求阶段。
 *
 * @param request_id 关联同一请求四个阶段的全局编号。
 * @param blockno xv6 文件系统块号。
 * @param write 0 表示读，1 表示写。
 * @param stage DISKTRACE_STAGE_*。
 * @param descriptor 三描述符链的首下标。
 * @param queue_index 当前事件对应的 avail/used 槽位；无槽位时为 -1。
 *
 * 调用者必须持有 disk.vdisk_lock。缓冲满时只累计 dropped，不阻塞真实 I/O。
 */
static void
append_trace_event_locked(uint64 request_id, uint64 blockno, int write,
                          int stage, int descriptor, int queue_index)
{
  struct disktrace_event event;

  if(!disk.trace_active)
    return;
  if(disk.trace_events >= DISKTRACE_MAX_EVENTS){
    disk.trace_dropped++;
    return;
  }

  memset(&event, 0, sizeof(event));
  event.seq = disk.trace_next_seq++;
  event.request_id = request_id;
  event.timestamp = r_time();
  event.blockno = blockno;
  event.stage = stage;
  event.write = write;
  event.descriptor = descriptor;
  event.queue_index = queue_index;
  disk.trace_events_buffer[disk.trace_events++] = event;
}

void
virtio_disk_init(void)
{
  uint32 status = 0;

  initlock(&disk.vdisk_lock, "virtio_disk");

  if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R(VIRTIO_MMIO_VERSION) != 1 ||
     *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
     *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
    panic("could not find virtio disk");
  }

  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(VIRTIO_MMIO_STATUS) = status;

  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  // negotiate features
  uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  // tell device that feature negotiation is complete.
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // tell device we're completely ready.
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  *R(VIRTIO_MMIO_GUEST_PAGE_SIZE) = PGSIZE;

  // initialize queue 0.
  *R(VIRTIO_MMIO_QUEUE_SEL) = 0;
  uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0)
    panic("virtio disk has no queue 0");
  if(max < NUM)
    panic("virtio disk max queue too short");
  *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;
  memset(disk.pages, 0, sizeof(disk.pages));
  *R(VIRTIO_MMIO_QUEUE_PFN) = ((uint64)disk.pages) >> PGSHIFT;

  // desc = pages -- num * VRingDesc
  // avail = pages + 0x40 -- 2 * uint16, then num * uint16
  // used = pages + 4096 -- 2 * uint16, then num * vRingUsedElem

  disk.desc = (struct VRingDesc *) disk.pages;
  disk.avail = (uint16*)(((char*)disk.desc) + NUM*sizeof(struct VRingDesc));
  disk.used = (struct UsedArea *) (disk.pages + PGSIZE);

  for(int i = 0; i < NUM; i++)
    disk.free[i] = 1;

  // 观察功能默认关闭，不改变正常块设备路径；请求编号跨 session 保持唯一。
  disk.trace_active = 0;
  disk.trace_dropped = 0;
  disk.trace_events = 0;
  disk.trace_next_seq = 1;
  disk.next_request_id = 1;

  // plic.c and trap.c arrange for interrupts from VIRTIO0_IRQ.
}

// find a free descriptor, mark it non-free, return its index.
static int
alloc_desc()
{
  for(int i = 0; i < NUM; i++){
    if(disk.free[i]){
      disk.free[i] = 0;
      return i;
    }
  }
  return -1;
}

// mark a descriptor as free.
static void
free_desc(int i)
{
  if(i >= NUM)
    panic("virtio_disk_intr 1");
  if(disk.free[i])
    panic("virtio_disk_intr 2");
  disk.desc[i].addr = 0;
  disk.free[i] = 1;
  wakeup(&disk.free[0]);
}

// free a chain of descriptors.
static void
free_chain(int i)
{
  while(1){
    free_desc(i);
    if(disk.desc[i].flags & VRING_DESC_F_NEXT)
      i = disk.desc[i].next;
    else
      break;
  }
}

static int
alloc3_desc(int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc();
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(idx[j]);
      return -1;
    }
  }
  return 0;
}

void
virtio_disk_rw(struct buf *b, int write)
{
  uint64 sector = b->blockno * (BSIZE / 512);

  acquire(&disk.vdisk_lock);

  // the spec says that legacy block operations use three
  // descriptors: one for type/reserved/sector, one for
  // the data, one for a 1-byte status result.

  // allocate the three descriptors.
  int idx[3];
  while(1){
    if(alloc3_desc(idx) == 0) {
      break;
    }
    sleep(&disk.free[0], &disk.vdisk_lock);
  }

  // format the three descriptors.
  // qemu's virtio-blk.c reads them.

  struct virtio_blk_outhdr {
    uint32 type;
    uint32 reserved;
    uint64 sector;
  } buf0;

  if(write)
    buf0.type = VIRTIO_BLK_T_OUT; // write the disk
  else
    buf0.type = VIRTIO_BLK_T_IN; // read the disk
  buf0.reserved = 0;
  buf0.sector = sector;

  // buf0 is on a kernel stack, which is not direct mapped,
  // thus the call to kvmpa().
  disk.desc[idx[0]].addr = (uint64) kvmpa((uint64) &buf0);
  disk.desc[idx[0]].len = sizeof(buf0);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next = idx[1];

  disk.desc[idx[1]].addr = (uint64) b->data;
  disk.desc[idx[1]].len = BSIZE;
  if(write)
    disk.desc[idx[1]].flags = 0; // device reads b->data
  else
    disk.desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
  disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next = idx[2];

  disk.info[idx[0]].status = 0;
  disk.desc[idx[2]].addr = (uint64) &disk.info[idx[0]].status;
  disk.desc[idx[2]].len = 1;
  disk.desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
  disk.desc[idx[2]].next = 0;

  // record struct buf and immutable request identity for virtio_disk_intr().
  uint64 request_id = disk.next_request_id++;
  b->disk = 1;
  disk.info[idx[0]].b = b;
  disk.info[idx[0]].request_id = request_id;
  disk.info[idx[0]].blockno = b->blockno;
  disk.info[idx[0]].write = write;
  append_trace_event_locked(request_id, b->blockno, write,
                            DISKTRACE_STAGE_SUBMIT, idx[0], -1);

  // avail[0] is flags
  // avail[1] tells the device how far to look in avail[2...].
  // avail[2...] are desc[] indices the device should process.
  // we only tell device the first index in our chain of descriptors.
  int avail_slot = disk.avail[1] % NUM;
  disk.avail[2 + avail_slot] = idx[0];
  __sync_synchronize();
  disk.avail[1] = disk.avail[1] + 1;

  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number
  append_trace_event_locked(request_id, b->blockno, write,
                            DISKTRACE_STAGE_QUEUED, idx[0], avail_slot);

  // Wait for virtio_disk_intr() to say request has finished.
  while(b->disk == 1) {
    sleep(b, &disk.vdisk_lock);
  }

  append_trace_event_locked(request_id, b->blockno, write,
                            DISKTRACE_STAGE_RETURN, idx[0], -1);
  disk.info[idx[0]].b = 0;
  disk.info[idx[0]].request_id = 0;
  free_chain(idx[0]);

  release(&disk.vdisk_lock);
}

void
virtio_disk_intr()
{
  acquire(&disk.vdisk_lock);

  while((disk.used_idx % NUM) != (disk.used->id % NUM)){
    int used_slot = disk.used_idx % NUM;
    int id = disk.used->elems[used_slot].id;

    if(disk.info[id].status != 0)
      panic("virtio_disk_intr status");

    append_trace_event_locked(disk.info[id].request_id,
                              disk.info[id].blockno,
                              disk.info[id].write,
                              DISKTRACE_STAGE_COMPLETE,
                              id, used_slot);
    disk.info[id].b->disk = 0;   // disk is done with buf
    wakeup(disk.info[id].b);

    disk.used_idx = (disk.used_idx + 1) % NUM;
  }
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  release(&disk.vdisk_lock);
}

/** 清空一次轨迹 session；不回退全局请求编号，也不影响在途 I/O。 */
void
virtio_disk_trace_reset(void)
{
  acquire(&disk.vdisk_lock);
  disk.trace_active = 0;
  disk.trace_dropped = 0;
  disk.trace_events = 0;
  disk.trace_next_seq = 1;
  release(&disk.vdisk_lock);
}

/** 开启驱动边界事件采样，默认 I/O 提交顺序和完成语义保持不变。 */
int
virtio_disk_trace_start(void)
{
  acquire(&disk.vdisk_lock);
  disk.trace_active = 1;
  release(&disk.vdisk_lock);
  return 0;
}

/** 关闭采样并保留最近一次 session，供用户态随后读取。 */
int
virtio_disk_trace_stop(void)
{
  acquire(&disk.vdisk_lock);
  disk.trace_active = 0;
  release(&disk.vdisk_lock);
  return 0;
}

/**
 * virtio_disk_trace_copy_snapshot 复制当前驱动轨迹到内核缓冲。
 *
 * @param snapshot 输出快照，必须位于内核地址空间。
 * @param max_events 调用者愿意接收的事件数，范围为 0..DISKTRACE_MAX_EVENTS。
 * @return 成功返回实际事件数；容量非法时返回 -1。
 */
int
virtio_disk_trace_copy_snapshot(struct disktrace_snapshot *snapshot,
                                int max_events)
{
  int n;

  if(max_events < 0 || max_events > DISKTRACE_MAX_EVENTS)
    return -1;

  acquire(&disk.vdisk_lock);
  n = disk.trace_events;
  if(n > max_events)
    n = max_events;

  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->version = DISKTRACE_VERSION;
  snapshot->events = n;
  snapshot->dropped = disk.trace_dropped + (disk.trace_events - n);
  snapshot->capacity = DISKTRACE_MAX_EVENTS;
  snapshot->active = disk.trace_active;
  memmove(snapshot->events_buffer, disk.trace_events_buffer,
          n * sizeof(struct disktrace_event));
  release(&disk.vdisk_lock);
  return n;
}
