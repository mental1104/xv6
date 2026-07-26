#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile static int started = 0;

// 并发入门探针由 spinlock.c 持有，只在启动 CPU 上初始化一次。
extern void concurrencylab_init(void);

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  if(cpuid() == 0){
    consoleinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    kinit();         // physical page allocator
    kvminit();       // create kernel page table
    // Goldfish RTC 和额外 virtio-mmio 插槽位于默认低半区映射之外，必须在
    // 启用分页前显式映射。缺少成员盘时寄存器读返回空插槽，而不是页表故障。
    kvmmap(RTC, RTC, PGSIZE, PTE_R | PTE_W);
    kvmmap(VIRTIO1, VIRTIO1, PGSIZE, PTE_R | PTE_W);
    kvmmap(VIRTIO2, VIRTIO2, PGSIZE, PTE_R | PTE_W);
    kvminithart();   // turn on paging
    procinit();      // process table
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    plicinit();      // set up interrupt controller
    plicinithart();  // ask PLIC for device interrupts
    binit();         // buffer cache
    iinit();         // inode cache
    fileinit();      // file table
    concurrencylab_init(); // explicit, inactive teaching probe
    virtio_disk_init(); // root disk plus optional teaching member disks
    raid1_init();    // independent RAID1 teaching layer; never replaces the root FS path
    userinit();      // first user process
    vma_init();
    __sync_synchronize();
    started = 1;
  } else {
    while(started == 0)
      ;
    __sync_synchronize();
    printf("hart %d starting\n", cpuid());
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    plicinithart();   // ask PLIC for device interrupts
  }

  scheduler();
}
