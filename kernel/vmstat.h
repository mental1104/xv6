#ifndef XV6_VMSTAT_H
#define XV6_VMSTAT_H

#define VMSTAT_ABI_VERSION 1

#define VMSTAT_BEGIN         1
#define VMSTAT_SAMPLE_SBRK   2
#define VMSTAT_SAMPLE_FORK   3
#define VMSTAT_SAMPLE_CHILD  4
#define VMSTAT_END           5

struct vmstat_counts {
  uint64 virtual_grow_pages;
  uint64 sbrk_eager_alloc_pages;
  uint64 lazy_materialized_pages;
  uint64 fork_va_scan_pages;
  uint64 fork_present_pages;
  uint64 fork_eager_copy_pages;
  uint64 fork_shared_map_pages;
  uint64 fork_cow_mark_pages;
  uint64 cow_copy_pages;
  uint64 final_present_pages;
};

struct vmstat_snapshot {
  uint64 abi_version;
  uint64 range_start;
  uint64 range_end;
  struct vmstat_counts total;
  struct vmstat_counts range;
};

#endif
