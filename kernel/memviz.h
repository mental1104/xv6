#ifndef XV6_MEMVIZ_H
#define XV6_MEMVIZ_H

#define MEMVIZ_CELLS 48

// memsnapshot() 支持的只读视图类型。
enum memviz_view {
  MEMVIZ_VIEW_USER = 1,
  MEMVIZ_VIEW_PHYS = 2,
  MEMVIZ_VIEW_KERNEL = 3,
};

// 一个字符单元覆盖连续物理页，并记录其中可立即分配的页数。
struct memviz_cell {
  uint total_pages;
  uint free_pages;
};

// memsnapshot() 返回的统一快照。
//
// 内核只填写地址和计数，不携带 ANSI 颜色或终端布局。用户态 renderer
// 根据 view 选择需要展示的字段。所有地址均使用当前 xv6 实现中的真实值。
struct memviz_snapshot {
  int view;
  int user_stack_valid;
  int kernel_stack_valid;
  int reserved;

  uint64 process_size;
  uint64 user_limit;
  uint64 image_start;
  uint64 image_end;
  uint64 stack_guard_start;
  uint64 stack_bottom;
  uint64 stack_top;
  uint64 user_sp;
  uint64 stack_used;
  uint64 stack_free;
  uint64 dynamic_start;

  uint64 kalloc_start;
  uint64 kalloc_end;
  uint64 total_pages;
  uint64 free_pages;
  uint64 used_pages;
  uint64 cpu_free_pages[NCPU];
  struct memviz_cell physical[MEMVIZ_CELLS];

  uint64 kernel_pagetable;
  uint64 kernel_sp;
  uint64 kernel_stack_guard_start;
  uint64 kernel_stack_bottom;
  uint64 kernel_stack_top;
  uint64 kernel_stack_used;
  uint64 kernel_stack_free;
  uint64 kernel_text_start;
  uint64 kernel_text_end;
  uint64 kernel_data_start;
  uint64 kernel_data_end;
  uint64 clint_start;
  uint64 clint_end;
  uint64 plic_start;
  uint64 plic_end;
  uint64 uart_start;
  uint64 uart_end;
  uint64 virtio_start;
  uint64 virtio_end;
  uint64 trampoline;
  uint64 user_mirror_start;
  uint64 user_mirror_end;
};

// 以下接口只由内核实现调用；用户态仅使用 memsnapshot() 系统调用。
void kalloc_mem_snapshot(struct memviz_snapshot *snapshot);
int memviz_snapshot(int view, struct memviz_snapshot *snapshot);

#endif
