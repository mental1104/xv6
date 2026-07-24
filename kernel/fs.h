// On-disk file system format.
// Both the kernel and user programs use this header file.

#define ROOTINO  1   // root i-number
#define BSIZE 1024  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

#define FSMAGIC 0x10203040

#define NDIRECT 9
#define NINDIRECT (BSIZE / sizeof(uint))
#define NDOUBLEINDIRECT ((uint64)NINDIRECT * NINDIRECT)
#define NTRIPLEINDIRECT (NDOUBLEINDIRECT * NINDIRECT)
#define NINDIRECT_LEVELS 3
#define MAXFILE ((uint64)NDIRECT + NINDIRECT + NDOUBLEINDIRECT + NTRIPLEINDIRECT)
#define MAXFILE_BYTES (MAXFILE * (uint64)BSIZE)

// On-disk inode structure. Keep this structure exactly 64 bytes so that the
// inode density and the surrounding disk layout stay stable.
//
// xv6 尚未实现权限和多用户字段。为不降低三级索引容量，mtime 复用按类型空闲的
// 既有字段：非设备 inode 使用 major/minor 的两个 16 位槽；设备 inode 必须保留
// major/minor，因此将 mtime 放入其逻辑上未使用的 size 高 32 位。
struct dinode {
  short type;                        // File type
  short major;                       // Device major, or non-device mtime high 16 bits
  short minor;                       // Device minor, or non-device mtime low 16 bits
  short nlink;                       // Number of links to inode in file system
  uint64 size;                       // File size; device mtime occupies high 32 bits
  uint addrs[NDIRECT+NINDIRECT_LEVELS]; // Direct and 1/2/3-level index roots
};

// Inodes per block.
#define IPB (BSIZE / sizeof(struct dinode))

// Block containing inode i.
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)

// Bitmap bits per block.
#define BPB (BSIZE*8)

// Block of free map containing bit for block b.
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};
