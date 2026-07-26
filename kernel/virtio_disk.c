//
// driver for qemu's virtio disk devices.
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

#define VIRTIO_DISK_COUNT 3
#define VIRTIO_MMIO_STRIDE 0x1000L

// 返回指定 virtio-mmio 插槽中的寄存器地址。
#define R(device, register) \
  ((volatile uint32 *)(VIRTIO0 + (device) * VIRTIO_MMIO_STRIDE + (register)))

/** 维护一个 legacy virtio-blk 设备的队列、请求和容量状态。 */
struct virtio_disk_state {
  // 描述符区必须物理连续且页对齐；当前 kalloc() 无法一次分配连续两页。
  char pages[2 * PGSIZE];
  struct VRingDesc *desc;
  uint16 *avail;
  struct UsedArea *used;

  char free[NUM];
  uint16 used_idx;

  // 中断只返回描述符链首索引，通过该表找到等待中的 struct buf。
  struct {
    struct buf *b;
    char status;
  } info[NUM];

  struct spinlock lock;
  uint64 capacity_blocks;
  int present;
} __attribute__((aligned(PGSIZE)));

static struct virtio_disk_state disks[VIRTIO_DISK_COUNT];

/**
 * 探测并初始化一个 legacy virtio-blk MMIO 插槽。
 *
 * @param device 设备下标；对应 virtio-mmio-bus.<device>。
 * @return 发现并完成初始化返回 0；插槽没有块设备返回 -1。
 */
static int
virtio_disk_init_device(int device)
{
  struct virtio_disk_state *disk = &disks[device];
  uint32 status = 0;

  initlock(&disk->lock, "virtio_disk");
  if(*R(device, VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R(device, VIRTIO_MMIO_VERSION) != 1 ||
     *R(device, VIRTIO_MMIO_DEVICE_ID) != 2 ||
     *R(device, VIRTIO_MMIO_VENDOR_ID) != 0x554d4551)
    return -1;

  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(device, VIRTIO_MMIO_STATUS) = status;

  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(device, VIRTIO_MMIO_STATUS) = status;

  // 每个设备独立协商 legacy 队列所支持的最小特性集合。
  uint64 features = *R(device, VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R(device, VIRTIO_MMIO_DRIVER_FEATURES) = features;

  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(device, VIRTIO_MMIO_STATUS) = status;

  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(device, VIRTIO_MMIO_STATUS) = status;

  *R(device, VIRTIO_MMIO_GUEST_PAGE_SIZE) = PGSIZE;
  *R(device, VIRTIO_MMIO_QUEUE_SEL) = 0;
  uint32 max = *R(device, VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0 || max < NUM)
    return -1;
  *R(device, VIRTIO_MMIO_QUEUE_NUM) = NUM;
  memset(disk->pages, 0, sizeof(disk->pages));
  *R(device, VIRTIO_MMIO_QUEUE_PFN) = ((uint64)disk->pages) >> PGSHIFT;

  disk->desc = (struct VRingDesc *)disk->pages;
  disk->avail = (uint16 *)((char *)disk->desc +
                          NUM * sizeof(struct VRingDesc));
  disk->used = (struct UsedArea *)(disk->pages + PGSIZE);
  for(int i = 0; i < NUM; i++)
    disk->free[i] = 1;

  // legacy virtio-blk 配置区以两个小端 32 位寄存器保存 512 字节扇区数。
  uint64 sectors = *R(device, VIRTIO_MMIO_CONFIG);
  sectors |= (uint64)*R(device, VIRTIO_MMIO_CONFIG + 4) << 32;
  disk->capacity_blocks = sectors / (BSIZE / 512);
  disk->present = 1;
  return 0;
}

/**
 * 初始化根文件系统设备和两个可选 RAID1 成员设备。
 *
 * 根设备 0 是 xv6 启动前置条件；设备 1、2 缺失只会让 RAID1 进入降级或不可用状态。
 */
void
virtio_disk_init(void)
{
  if(virtio_disk_init_device(0) < 0)
    panic("could not find virtio root disk");

  for(int device = 1; device < VIRTIO_DISK_COUNT; device++)
    virtio_disk_init_device(device);
}

/**
 * 查询一个 virtio 块设备是否已经完成初始化。
 *
 * @param device 设备下标。
 * @return 已发现返回 1，越界或缺失返回 0。
 */
int
virtio_disk_present(int device)
{
  return device >= 0 && device < VIRTIO_DISK_COUNT &&
         disks[device].present;
}

/**
 * 查询一个 virtio 块设备可寻址的 xv6 块数量。
 *
 * @param device 设备下标。
 * @return 在线设备容量；越界或缺失返回 0。
 */
uint64
virtio_disk_capacity(int device)
{
  if(!virtio_disk_present(device))
    return 0;
  return disks[device].capacity_blocks;
}

/** 从指定设备分配一个描述符并标记为占用。 */
static int
alloc_desc(struct virtio_disk_state *disk)
{
  for(int i = 0; i < NUM; i++){
    if(disk->free[i]){
      disk->free[i] = 0;
      return i;
    }
  }
  return -1;
}

/** 释放指定设备的一个描述符，并唤醒等待描述符的请求。 */
static void
free_desc(struct virtio_disk_state *disk, int index)
{
  if(index >= NUM)
    panic("virtio_disk_intr 1");
  if(disk->free[index])
    panic("virtio_disk_intr 2");
  disk->desc[index].addr = 0;
  disk->free[index] = 1;
  wakeup(&disk->free[0]);
}

/** 释放以 index 开始的完整描述符链。 */
static void
free_chain(struct virtio_disk_state *disk, int index)
{
  while(1){
    uint16 flags = disk->desc[index].flags;
    uint16 next = disk->desc[index].next;
    free_desc(disk, index);
    if(flags & VRING_DESC_F_NEXT)
      index = next;
    else
      break;
  }
}

/** 原子地尝试分配 legacy 块请求需要的三个描述符。 */
static int
alloc3_desc(struct virtio_disk_state *disk, int *indexes)
{
  for(int i = 0; i < 3; i++){
    indexes[i] = alloc_desc(disk);
    if(indexes[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(disk, indexes[j]);
      return -1;
    }
  }
  return 0;
}

/**
 * 在指定 virtio 设备上同步读写一个 xv6 缓冲块。
 *
 * @param device 设备下标，0 为根文件系统，1、2 为可选教学成员盘。
 * @param buffer 提供块号和 BSIZE 字节数据；请求期间由设备暂时拥有。
 * @param write 非零表示写入，零表示读取。
 * @return 请求成功返回 0；设备缺失、块越界或设备状态错误返回 -1。
 */
int
virtio_disk_rw_device(int device, struct buf *buffer, int write)
{
  if(!virtio_disk_present(device) ||
     buffer->blockno >= disks[device].capacity_blocks)
    return -1;

  struct virtio_disk_state *disk = &disks[device];
  uint64 sector = buffer->blockno * (BSIZE / 512);
  acquire(&disk->lock);

  int indexes[3];
  while(alloc3_desc(disk, indexes) < 0)
    sleep(&disk->free[0], &disk->lock);

  struct virtio_blk_outhdr {
    uint32 type;
    uint32 reserved;
    uint64 sector;
  } request;

  request.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
  request.reserved = 0;
  request.sector = sector;

  // request 位于进程内核栈，必须转换为设备可见的物理地址。
  disk->desc[indexes[0]].addr = (uint64)kvmpa((uint64)&request);
  disk->desc[indexes[0]].len = sizeof(request);
  disk->desc[indexes[0]].flags = VRING_DESC_F_NEXT;
  disk->desc[indexes[0]].next = indexes[1];

  disk->desc[indexes[1]].addr = (uint64)buffer->data;
  disk->desc[indexes[1]].len = BSIZE;
  disk->desc[indexes[1]].flags = write ? 0 : VRING_DESC_F_WRITE;
  disk->desc[indexes[1]].flags |= VRING_DESC_F_NEXT;
  disk->desc[indexes[1]].next = indexes[2];

  disk->info[indexes[0]].status = 0xff;
  disk->desc[indexes[2]].addr =
    (uint64)&disk->info[indexes[0]].status;
  disk->desc[indexes[2]].len = 1;
  disk->desc[indexes[2]].flags = VRING_DESC_F_WRITE;
  disk->desc[indexes[2]].next = 0;

  buffer->disk = 1;
  disk->info[indexes[0]].b = buffer;
  disk->avail[2 + (disk->avail[1] % NUM)] = indexes[0];
  __sync_synchronize();
  disk->avail[1] = disk->avail[1] + 1;
  *R(device, VIRTIO_MMIO_QUEUE_NOTIFY) = 0;

  while(buffer->disk == 1)
    sleep(buffer, &disk->lock);

  int success = disk->info[indexes[0]].status == 0;
  disk->info[indexes[0]].b = 0;
  free_chain(disk, indexes[0]);
  release(&disk->lock);
  return success ? 0 : -1;
}

/**
 * 保持文件系统原有单设备接口；根盘 I/O 失败仍属于内核致命错误。
 *
 * @param buffer 文件系统缓冲块。
 * @param write 非零表示写入，零表示读取。
 */
void
virtio_disk_rw(struct buf *buffer, int write)
{
  if(virtio_disk_rw_device(0, buffer, write) < 0)
    panic("virtio root disk io");
}

/**
 * 完成指定 virtio 设备已经由 QEMU 放入 used ring 的请求。
 *
 * @param device 由 PLIC IRQ 映射得到的设备下标。
 */
void
virtio_disk_intr(int device)
{
  if(!virtio_disk_present(device))
    return;

  struct virtio_disk_state *disk = &disks[device];
  acquire(&disk->lock);

  while((disk->used_idx % NUM) != (disk->used->id % NUM)){
    int id = disk->used->elems[disk->used_idx].id;
    struct buf *buffer = disk->info[id].b;
    if(buffer != 0){
      buffer->disk = 0;
      wakeup(buffer);
    }
    disk->used_idx = (disk->used_idx + 1) % NUM;
  }
  *R(device, VIRTIO_MMIO_INTERRUPT_ACK) =
    *R(device, VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  release(&disk->lock);
}
