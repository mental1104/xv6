#ifndef XV6_MEMVIZ_H
#define XV6_MEMVIZ_H

#define MEMVIZ_CELLS 48
#define MEMVIZ_PTE_ENTRIES 24
#define MEMVIZ_PT_USAGE_PAGES 16
#define MEMVIZ_PT_USAGE_CELLS 64

// memsnapshot() 支持的只读视图类型。
enum memviz_view {
  MEMVIZ_VIEW_USER = 1,
  MEMVIZ_VIEW_PHYS = 2,
  MEMVIZ_VIEW_KERNEL = 3,
  MEMVIZ_VIEW_PAGETABLE = 4,
};

// 页表观察条目所属地址空间。
enum memviz_pte_space {
  MEMVIZ_PTE_SPACE_USER = 1,
  MEMVIZ_PTE_SPACE_KERNEL = 2,
};

// 页表观察条目的语义角色；用户态 renderer 只根据角色选择标签。
enum memviz_pte_role {
  MEMVIZ_PTE_ROLE_ELF_FIRST = 1,
  MEMVIZ_PTE_ROLE_ELF_LAST = 2,
  MEMVIZ_PTE_ROLE_GUARD = 3,
  MEMVIZ_PTE_ROLE_USER_STACK = 4,
  MEMVIZ_PTE_ROLE_DYNAMIC = 5,
  MEMVIZ_PTE_ROLE_USER_MIRROR = 6,
  MEMVIZ_PTE_ROLE_KERNEL_STACK_GUARD = 7,
  MEMVIZ_PTE_ROLE_KERNEL_STACK = 8,
  MEMVIZ_PTE_ROLE_TRAMPOLINE = 9,
  MEMVIZ_PTE_ROLE_KERNEL_TEXT = 10,
  MEMVIZ_PTE_ROLE_RAM_DIRECT = 11,
  MEMVIZ_PTE_ROLE_UART = 12,
  MEMVIZ_PTE_ROLE_VIRTIO = 13,
  MEMVIZ_PTE_ROLE_CLINT = 14,
  MEMVIZ_PTE_ROLE_PLIC = 15,
};

// 一个字符单元覆盖连续物理页，并记录其中可立即分配的页数。
struct memviz_cell {
  uint total_pages;
  uint free_pages;
};

// 一个页表观察行记录一个代表性 VA 的叶子 PTE。
struct memviz_pte_level {
  int level;
  int index;
  int present;
  int reserved;
  uint64 pte;
  uint64 pa;
  uint64 flags;
};

struct memviz_pte_entry {
  int space;
  int role;
  int present;
  int reserved;
  uint64 va;
  uint64 pa;
  uint64 pte;
  uint64 flags;
  struct memviz_pte_level levels[3];
};

// 一个页表页内部的 PTE 槽占用压缩单元。
struct memviz_pt_usage_cell {
  uint total_entries;
  uint used_entries;
};

// 一个已分配页表页的槽位占用摘要。
struct memviz_pt_usage_page {
  int space;
  int level;
  int reserved0;
  int reserved1;
  uint64 pa;
  uint64 used_entries;
  uint64 total_entries;
  struct memviz_pt_usage_cell cells[MEMVIZ_PT_USAGE_CELLS];
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

  uint64 user_pagetable;
  uint64 pagetable_entry_count;
  struct memviz_pte_entry pagetable_entries[MEMVIZ_PTE_ENTRIES];
  uint64 pagetable_usage_count;
  struct memviz_pt_usage_page pagetable_usage[MEMVIZ_PT_USAGE_PAGES];
};

// 以下接口只由内核实现调用；用户态仅使用 memsnapshot() 系统调用。
void kalloc_mem_snapshot(struct memviz_snapshot *snapshot);
int memviz_snapshot(int view, struct memviz_snapshot *snapshot);

#endif
