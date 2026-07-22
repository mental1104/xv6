K=kernel
U=user
T=tests/guest
H=tests/host

OBJS = \
  $K/entry.o \
  $K/start.o \
  $K/console.o \
  $K/printf.o \
  $K/uart.o \
  $K/kalloc.o \
  $K/spinlock.o \
  $K/string.o \
  $K/main.o \
  $K/vm.o \
  $K/vmcopyin.o \
  $K/proc.o \
  $K/swtch.o \
  $K/trampoline.o \
  $K/trap.o \
  $K/syscall.o \
  $K/sysproc.o \
  $K/bio.o \
  $K/fs.o \
  $K/log.o \
  $K/sleeplock.o \
  $K/file.o \
  $K/pipe.o \
  $K/exec.o \
  $K/sysfile.o \
  $K/kernelvec.o \
  $K/plic.o \
  $K/virtio_disk.o \
  $K/sprintf.o \
  $K/vma.o

# riscv64-unknown-elf- or riscv64-linux-gnu-
# perhaps in /opt/riscv/bin
#TOOLPREFIX =

ifndef TOOLPREFIX
TOOLPREFIX := $(shell if riscv64-unknown-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
  then echo 'riscv64-unknown-elf-'; \
  elif riscv64-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
  then echo 'riscv64-linux-gnu-'; \
  elif riscv64-unknown-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
  then echo 'riscv64-unknown-linux-gnu-'; \
  else echo "***" 1>&2; \
  echo "*** Error: Couldn't find a riscv64 version of GCC/binutils." 1>&2; \
  echo "*** To turn off this error, run 'make TOOLPREFIX= ...'." 1>&2; \
  echo "***" 1>&2; exit 1; fi)
endif

QEMU = qemu-system-riscv64
PYTHON ?= python3

CC = $(TOOLPREFIX)gcc
AS = $(TOOLPREFIX)gas
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb
CFLAGS += -MD
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding -fno-common -nostdlib -mno-relax
CFLAGS += -I.
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

LDFLAGS = -z max-page-size=4096

$K/kernel: $(OBJS) $K/kernel.ld $U/initcode
	$(LD) $(LDFLAGS) -T $K/kernel.ld -o $K/kernel $(OBJS)
	$(OBJDUMP) -S $K/kernel > $K/kernel.asm
	$(OBJDUMP) -t $K/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $K/kernel.sym

$U/initcode: $U/initcode.S
	$(CC) $(CFLAGS) -march=rv64g -nostdinc -I. -Ikernel -c $U/initcode.S -o $U/initcode.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o $U/initcode.out $U/initcode.o
	$(OBJCOPY) -S -O binary $U/initcode.out $U/initcode
	$(OBJDUMP) -S $U/initcode.o > $U/initcode.asm

tags: $(OBJS) _init
	etags *.S *.c

ULIB = $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o

_%: %.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

# 测试源代码统一保存在 tests/guest；生成的 xv6 用户程序仍写入 user/_*
# 以保持 mkfs 输入格式和 guest 命令名不变。
$U/_%: $T/%.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $T/$*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $T/$*.sym

$U/_trace: $U/trace.o $U/tracemask.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $U/trace.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $U/trace.sym

$U/_tracemasktest: $T/tracemasktest.o $U/tracemask.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $T/tracemasktest.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $T/tracemasktest.sym

$U/usys.S: $U/usys.pl
	perl $U/usys.pl > $U/usys.S

$U/usys.o: $U/usys.S
	$(CC) $(CFLAGS) -c -o $U/usys.o $U/usys.S

$U/_forktest: $T/forktest.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $U/_forktest $T/forktest.o $U/ulib.o $U/usys.o
	$(OBJDUMP) -S $U/_forktest > $T/forktest.asm

$U/_lab1test: $T/lab1test.o $T/testlib.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $T/lab1test.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $T/lab1test.sym

$U/_uthreadtest: $T/uthreadtest.o $T/testlib.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $T/uthreadtest.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $T/uthreadtest.sym

$U/uthread_switch.o: $U/uthread_switch.S
	$(CC) $(CFLAGS) -c -o $U/uthread_switch.o $U/uthread_switch.S

$U/_uthread: $U/uthread.o $U/uthread_switch.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $U/uthread.o $U/uthread_switch.o $(ULIB)
	$(OBJDUMP) -S $U/_uthread > $U/uthread.asm

# xargstest.sh 是测试输入脚本，源文件放在 tests/guest；构建时复制到 user/
# 只是为了满足 mkfs 对输入路径不能包含多级目录的既有约束。
$U/xargstest.sh: $T/xargstest.sh
	cp $< $@

mkfs/mkfs: mkfs/mkfs.c $K/fs.h $K/param.h
	gcc -Werror -Wall -I. -o mkfs/mkfs mkfs/mkfs.c

.PRECIOUS: %.o

UPROGS=\
	$U/_cat\
	$U/_echo\
	$U/_grep\
	$U/_init\
	$U/_kill\
	$U/_ln\
	$U/_ls\
	$U/_mkdir\
	$U/_rm\
	$U/_sh\
	$U/_wc\
	$U/_zombie\
	$U/_pingpong\
	$U/_sleep\
	$U/_primes\
	$U/_find\
	$U/_xargs\
	$U/_trace\
	$U/_call\
	$U/_uthread\
	$U/_forktest\
	$U/_stressfs\
	$U/_usertests\
	$U/_grind\
	$U/_tracemasktest\
	$U/_sysinfotest\
	$U/_bttest\
	$U/_alarmtest\
	$U/_lazytests\
	$U/_cowtest\
	$U/_bigfile\
	$U/_symlinktest\
	$U/_mmaptest\
	$U/_lab1test\
	$U/_tracesmoke\
	$U/_uthreadtest\
	$U/_xv6test

UEXTRA = $U/xargstest.sh

fs.img: mkfs/mkfs README $(UEXTRA) $(UPROGS)
	mkfs/mkfs fs.img README $(UEXTRA) $(UPROGS)

-include kernel/*.d user/*.d tests/guest/*.d

clean:
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
		*/*.o */*.d */*.asm */*.sym \
		$T/*.o $T/*.d $T/*.asm $T/*.sym \
		$U/initcode $U/initcode.out $K/kernel fs.img \
		mkfs/mkfs .gdbinit $U/usys.S $(UPROGS) $(UEXTRA) ph barrier

GDBPORT = $(shell expr `id -u` % 5000 + 25000)
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)
ifndef CPUS
CPUS := 3
endif

QEMUOPTS = -machine virt -bios none -kernel $K/kernel -m 128M -smp $(CPUS) -nographic
QEMUOPTS += -drive file=fs.img,if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMUOPTS += $(QEMUEXTRA)

qemu: $K/kernel fs.img
	$(QEMU) $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl-riscv
	sed "s/:1234/:$(GDBPORT)/" < $^ > $@

qemu-gdb: $K/kernel .gdbinit fs.img
	@echo "*** Now run 'gdb' in another window." 1>&2
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)

gdb:
	riscv64-linux-gnu-gdb kernel/kernel

ph: $H/ph.c
	gcc -o ph -g -O2 $H/ph.c -pthread

barrier: $H/barrier.c
	gcc -o barrier -g -O2 $H/barrier.c -pthread

# 默认开发入口：先自测 Python runner，再由同一 Python 入口启动 QEMU
# 并执行 PR 级回归。使用子 make 保证即使外层带 -j，两阶段仍按顺序执行。
test:
	$(MAKE) test-unit
	$(MAKE) test-integration CPUS=$(CPUS)

# Unit-test the grader itself. This target never boots QEMU and does not need
# a built xv6 image.
test-unit:
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py' -v

test-grader: test-unit

# Integration/system tests: boot a fresh QEMU snapshot for every atomic suite
# and validate xv6 through its user-visible behavior.
test-integration: $K/kernel fs.img
	$(PYTHON) tests/run.py --suite pr --cpus $(CPUS)

test-labs: test-integration

test-usertests: $K/kernel fs.img
	$(PYTHON) tests/run.py --suite usertests-full --cpus $(CPUS)

test-full: $K/kernel fs.img
	$(PYTHON) tests/run.py --suite full --cpus $(CPUS)

test-suite: $K/kernel fs.img
	@test -n "$(SUITE)" || (echo "usage: make test-suite SUITE=<suite> [CPUS=<n>]"; exit 2)
	$(PYTHON) tests/run.py --suite $(SUITE) --cpus $(CPUS)

.PHONY: clean qemu qemu-gdb gdb ph barrier test test-unit test-grader \
	test-integration test-labs test-usertests test-full test-suite
