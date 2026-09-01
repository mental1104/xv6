FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# 与主线 CI 使用相同的 Ubuntu 软件源和 RISC-V 工具链，避免本地环境漂移。
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        binutils-riscv64-linux-gnu \
        build-essential \
        ca-certificates \
        gcc-riscv64-linux-gnu \
        gdb-multiarch \
        python3 \
        python3-pexpect \
        qemu-system-misc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace/xv6

COPY . .

# 同时生成 kernel 与 fs.img，确保镜像在启动 QEMU 前已包含完整 guest 启动产物。
RUN make clean \
    && make -j"$(nproc)" kernel/kernel fs.img

# 默认通过 QEMU 的无图形串口终端启动 RISC-V xv6。
CMD ["make", "qemu"]
