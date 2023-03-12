struct VMA {
  uint64 addr;
  uint64 length;
  uint64 offset;
  int prot;
  int flags;
  struct file *file;
  int used;
};
