# syntax=docker/dockerfile:1

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# 代理参数由 docker compose build.args 从当前 shell 透传；未设置时保持为空，
# 让 Docker 构建继续使用宿主机或 Docker daemon 的默认网络配置。
ARG HTTP_PROXY
ARG http_proxy
ARG HTTPS_PROXY
ARG https_proxy
ARG NO_PROXY
ARG no_proxy

# 与主线 CI 使用相同的 Ubuntu 软件源和 RISC-V 工具链，避免本地环境漂移。
# 保留 APT 缓存并重试短暂的代理错误，避免大体积工具链下载失败后从头开始。
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    rm -f /etc/apt/apt.conf.d/docker-clean \
    && apt-get -o Acquire::Retries=10 -o Acquire::http::Timeout=180 update \
    && apt-get -o Acquire::Retries=10 -o Acquire::http::Timeout=180 install -y --no-install-recommends \
        binutils-riscv64-linux-gnu \
        build-essential \
        ca-certificates \
        gcc-riscv64-linux-gnu \
        gdb-multiarch \
        python3 \
        python3-pexpect \
        qemu-system-misc

WORKDIR /workspace/xv6

COPY . .

# 同时生成 kernel 与 fs.img，确保镜像在启动 QEMU 前已包含完整 guest 启动产物。
RUN make clean \
    && make -j"$(nproc)" kernel/kernel fs.img

# 默认通过 QEMU 的无图形串口终端启动 RISC-V xv6。
CMD ["make", "qemu"]
