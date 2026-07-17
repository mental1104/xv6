LAB=lazy

override OBJS = \
  $(K)/entry.o \
  $(K)/start.o \
  $(K)/console.o \
  $(K)/printf.o \
  $(K)/uart.o \
  $(K)/kalloc.o \
  $(K)/spinlock.o \
  $(K)/string.o \
  $(K)/main.o \
  $(K)/vm.o \
  $(K)/proc.o \
  $(K)/swtch.o \
  $(K)/trampoline.o \
  $(K)/trap.o \
  $(K)/syscall.o \
  $(K)/sysproc.o \
  $(K)/vmstat.o \
  $(K)/bio.o \
  $(K)/fs.o \
  $(K)/log.o \
  $(K)/sleeplock.o \
  $(K)/file.o \
  $(K)/pipe.o \
  $(K)/exec.o \
  $(K)/sysfile.o \
  $(K)/kernelvec.o \
  $(K)/plic.o \
  $(K)/virtio_disk.o

override UPROGS = \
  $(U)/_cat \
  $(U)/_echo \
  $(U)/_forktest \
  $(U)/_grep \
  $(U)/_init \
  $(U)/_kill \
  $(U)/_ln \
  $(U)/_ls \
  $(U)/_mkdir \
  $(U)/_rm \
  $(U)/_sh \
  $(U)/_stressfs \
  $(U)/_usertests \
  $(U)/_grind \
  $(U)/_wc \
  $(U)/_zombie \
  $(U)/_lazytests \
  $(U)/_vmbench
