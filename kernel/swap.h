#ifndef XV6_SWAP_H
#define XV6_SWAP_H

#include "types.h"

struct proc;

// RISC-V 为软件保留 PTE 的第 8、9 位；第 8 位已用于 COW，本机制使用第 9 位。
#define PTE_SWAP (1L << 9)

// 教学镜像在文件系统块范围之后追加 128 个原始块；4 个 1 KiB 块保存一页。
#define SWAP_BLOCKS 128
#define SWAP_PAGE_BLOCKS 4
#define SWAP_SLOTS (SWAP_BLOCKS / SWAP_PAGE_BLOCKS)

// swapquery() 对一个用户虚拟页返回的稳定状态。
enum swap_page_state {
  SWAP_PAGE_INVALID = 0,
  SWAP_PAGE_LAZY = 1,
  SWAP_PAGE_RESIDENT = 2,
  SWAP_PAGE_SWAPPED = 3,
};

/** 描述一个用户虚拟页当前是否驻留，以及它的 swap 后备位置。 */
struct swap_page_info {
  uint64 va;       // 页对齐用户虚拟地址。
  uint64 flags;    // resident 或 swapped leaf 保存的原始低位 PTE 权限。
  int state;       // enum swap_page_state。
  int slot;        // swapped 时为 [0, SWAP_SLOTS)，其他状态为 -1。
};

/** 初始化 swap slot 元数据；启动阶段只初始化锁，不执行磁盘 I/O。 */
void swapinit(void);

/**
 * 将当前进程一个已驻留的匿名用户页写入 swap 并释放物理页框。
 *
 * @param p 目标进程，必须是当前执行系统调用的进程。
 * @param va 目标用户地址；函数内部按页向下对齐。
 * @return 成功返回 0；地址、页状态、slot 或页面类型不满足约束时返回 -1。
 */
int swapout_page(struct proc *p, uint64 va);

/**
 * 在 page fault 或内核用户指针访问路径中恢复 swapped 页。
 *
 * @param p 拥有目标用户页表和 kernel alias 页表的进程。
 * @param va faulting 用户地址；函数内部按页向下对齐。
 * @return 恢复成功返回 0；目标不是 swapped 页返回 1；恢复失败返回 -1。
 */
int swapin_page(struct proc *p, uint64 va);

/**
 * 查询用户虚拟页的 lazy、resident、swapped 或 invalid 状态。
 *
 * @param p 当前进程。
 * @param va 目标用户地址；允许尚未建立 leaf PTE。
 * @param info 接收页状态，不接管任何内核资源。
 * @return 参数有效时返回 0；地址越界或输出为空时返回 -1。
 */
int swapquery_page(struct proc *p, uint64 va, struct swap_page_info *info);

/** 为 fork 后共享的 swapped PTE 增加一个 slot 引用。 */
int swapretain_pte(uint64 pte);

/** 为被缩容、exec 或 exit 移除的 swapped PTE 释放一个 slot 引用。 */
void swaprelease_pte(uint64 pte);

#endif
