struct VMA {
  char* addr;
  uint64 length;
  char prot;
  char flags;
  struct file *file;
};