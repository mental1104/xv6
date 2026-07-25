#ifndef XV6_SWAP_H
#define XV6_SWAP_H

#define NSWAP 64
#define SWAPFILE_PATH "/var/xv6.swap"

// RISC-V reserves PTE bits 8 and 9 for supervisor software. COW already uses
// bit 8; the teaching swap mechanism uses bit 9 while PTE_V remains clear.
#define PTE_SWAP (1L << 9)
#define PTE_IS_SWAPPED(pte) (((pte) & PTE_SWAP) != 0 && ((pte) & PTE_V) == 0)
#define SWAP_SLOT_TO_PTE(slot) (((uint64)((slot) + 1)) << 10)
#define PTE_TO_SWAP_SLOT(pte) ((int)(((pte) >> 10) - 1))

enum swap_page_state {
  SWAP_PAGE_UNMAPPED = 0,
  SWAP_PAGE_RESIDENT = 1,
  SWAP_PAGE_SWAPPED = 2,
};

/**
 * Snapshot of the deterministic teaching swap mechanism.
 *
 * The global counters are observability oracles. page_state and slot describe
 * one caller-selected virtual page without touching its contents or causing a
 * page fault.
 */
struct swap_info {
  uint total_slots;
  uint used_slots;
  uint64 page_outs;
  uint64 page_ins;
  int page_state;
  int slot;
};

#endif
