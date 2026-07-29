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

# 镜像构建阶段执行一次干净编译，确保镜像不依赖宿主机生成的 xv6 产物。
RUN make clean \
    && make -j"$(nproc)"

# 默认通过 QEMU 的无图形串口终端启动 RISC-V xv6。
CMD ["make", "qemu"]
