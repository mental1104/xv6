DOCKER ?= docker
DOCKER_COMPOSE ?= $(DOCKER) compose
DOCKER_IMAGE ?= xv6-dev

# 构建包含当前源码、RISC-V 工具链和 QEMU 的可复现 xv6 镜像。
docker-build:
	$(DOCKER_COMPOSE) build

# 使用 docker compose 管理 QEMU 参数、资源限制和交互终端。
docker-qemu:
	$(DOCKER_COMPOSE) run --rm xv6

# 进入镜像内的源码目录，便于复验工具链和手工执行构建命令。
docker-shell:
	$(DOCKER_COMPOSE) run --rm --entrypoint /bin/bash xv6

# 在镜像内执行仓库现有的默认回归入口，并透传测试退出码。
docker-test:
	$(DOCKER_COMPOSE) run --rm test

.PHONY: docker-build docker-qemu docker-shell docker-test
