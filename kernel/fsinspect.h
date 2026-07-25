#ifndef XV6_KERNEL_FSINSPECT_H
#define XV6_KERNEL_FSINSPECT_H

#define FSINSPECT_VERSION 1
#define FSINSPECT_GLOBAL_FD (-1)

/** 描述 buffer cache 自启动以来累计的公开观察计数。 */
struct fsinspect_cache_stats {
  uint64 requests;
  uint64 hits;
  uint64 misses;
  uint64 disk_reads;
  uint64 disk_writes;
  uint64 steals;
};

/** 描述物理 redo log 自启动以来累计的公开观察计数。 */
struct fsinspect_log_stats {
  uint64 commits;
  uint64 committed_blocks;
  uint64 recoveries;
};

/**
 * 提供一个只读文件系统观察结果，连接磁盘布局、资源分配、inode 块映射、日志和缓存。
 *
 * has_inode 为零时仅有全局字段有效；非零时 inode 字段来自调用者提供的已打开
 * 普通文件或设备 inode。direct_first、direct_last 与 indirect_first 分别观察逻辑块
 * 0、NDIRECT-1 和 NDIRECT，未建立映射时保持零。
 *
 * 全局计数通过顺序扫描得到，不冻结其他 CPU 的文件系统操作，因此不是原子一致性
 * 快照。测试必须隔离其他写入者，并比较同一启动实例中的前后增量。
 */
struct fsinspect_snapshot {
  uint version;
  uint has_inode;

  uint total_blocks;
  uint data_blocks;
  uint data_start;
  uint inode_count;
  uint log_start;
  uint log_blocks;
  uint inode_start;
  uint bitmap_start;

  uint64 allocated_inodes;
  uint64 allocated_blocks;

  uint inode_number;
  short inode_type;
  short inode_nlink;
  uint64 inode_size;
  uint direct_first;
  uint direct_last;
  uint indirect_root;
  uint indirect_first;

  struct fsinspect_cache_stats cache;
  struct fsinspect_log_stats log;
};

#endif
