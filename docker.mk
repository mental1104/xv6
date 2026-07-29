DOCKER ?= docker
DOCKER_IMAGE ?= xv6-dev

# 构建包含当前源码、RISC-V 工具链和 QEMU 的可复现 xv6 镜像。
docker-build:
	$(DOCKER) build --pull -t $(DOCKER_IMAGE) .

# 使用交互式串口启动镜像内的 QEMU；退出 QEMU 时容器随之删除。
docker-qemu: docker-build
	$(DOCKER) run --rm -it $(DOCKER_IMAGE)

# 进入镜像内的源码目录，便于复验工具链和手工执行构建命令。
docker-shell: docker-build
	$(DOCKER) run --rm -it --entrypoint /bin/bash $(DOCKER_IMAGE)

# 在镜像内执行仓库现有的默认回归入口，并透传测试退出码。
docker-test: docker-build
	$(DOCKER) run --rm $(DOCKER_IMAGE) make test

.PHONY: docker-build docker-qemu docker-shell docker-test
