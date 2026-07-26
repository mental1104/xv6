# RAID1 教学实验是显式 opt-in 构建层，不改变普通 make 的用户程序集合。
CFLAGS += -DXV6_RAID1
OBJS += $K/sysraid1.o $K/raid1.o
UPROGS += $U/_raid1test

# 这些依赖在主 Makefile 的规则解析后补入，确保新增对象和用户程序参与构建。
$K/kernel: $K/sysraid1.o $K/raid1.o
fs.img: $U/_raid1test

ifneq ($(strip $(RAID1_MEMBER0)),)
QEMUOPTS += -drive file=$(RAID1_MEMBER0),if=none,format=raw,id=raid10
QEMUOPTS += -device virtio-blk-device,drive=raid10,bus=virtio-mmio-bus.1
endif
ifneq ($(strip $(RAID1_MEMBER1)),)
QEMUOPTS += -drive file=$(RAID1_MEMBER1),if=none,format=raw,id=raid11
QEMUOPTS += -device virtio-blk-device,drive=raid11,bus=virtio-mmio-bus.2
endif

# 创建两个真实 raw 成员镜像，并跨四次 QEMU 启动验证降级、损坏、自愈与重启一致性。
raid1test: $K/kernel fs.img
	$(PYTHON) tests/raid1_experiment.py --cpus $(CPUS) --artifacts artifacts/raid1-cpu$(CPUS)

.PHONY: raid1test
