---
name: xv6-guest-first-testing
description: 在 mental1104/xv6 中新增、迁移、整理或维护测试时使用。适用于“新增测试”“补测试”“迁移到 xv6test”“调整 make test”“修改 tests/run.py”“为某个 Lab 增加回归”等请求。要求所有测试源码位于 tests/，user/ 与 kernel/ 只保存被测实现；优先让 xv6 guest 持有测试语义，Python 只负责 QEMU 编排、超时和日志；禁止新增或修改 GitHub Actions workflow；所有持久化改动必须通过 Issue、分支和 Draft PR。
---

# xv6 Guest-first 测试最佳实践

## 核心模型

本仓库的测试边界固定为：

```text
user/、kernel/
└── 被测实现

tests/guest/
├── guest 测试源码
├── 场景构造与断言
├── 测试 helper
└── xv6test registry

tests/run.py
├── QEMU 生命周期
├── snapshot 隔离
├── timeout
├── 原始日志
└── suite 编排
```

必须保持：

> 测试源码属于 `tests/`；测试语义属于 guest；Python 只拥有测试基础设施语义。

不得在 `user/` 或 `kernel/` 新增 `*test.c`、测试专用脚本或 runner。编译后生成在 `user/_<name>` 的文件是构建产物，不代表源码所有权。

## 触发条件

处理以下任务时必须使用本 Skill：

- 新增 xv6 测试；
- 为 Lab1-Lab10 补回归；
- 迁移 `usertests`、`lazytests`、`cowtest` 等历史测试；
- 修改 `tests/guest/xv6test.c`；
- 修改 `tests/run.py`、`tests/test_runner.py` 或 `make test`；
- 修复测试退出状态、假阳性、资源泄漏或 QEMU 超时；
- 设计新内核机制的自动验收；
- 将测试源码从 `user/` / `kernel/` 迁入 `tests/`。

## 开始前调查

AI 开工前必须：

1. 记录最新 `main` Commit SHA。
2. 读取仓库实际文件：`Makefile`、`tests/README.md`、`tests/guest/xv6test.c`、`tests/run.py`、`tests/test_runner.py`、目标实现和相邻测试。
3. 读取仓库中的 `AGENTS.md`、`CONTRIBUTING.md`；不存在时明确记录未找到。
4. 读取强制代码注释 Skill。
5. 搜索已有相同或近似测试，避免重复执行或重复断言。
6. 检查所有成功和失败路径是否可靠传播退出状态。
7. 确认测试类型和应使用的隔离边界。
8. 建立或更新聚焦 Issue，从最新 `main` 创建/同步分支，最终保持 Draft PR。

不得按原版 xv6、教材或其他分支记忆猜测当前实现。

## 测试类型决策树

### A. 已有 `usertests` 子用例

```text
已有用例
→ 检查失败路径和退出状态
→ 在 xv6test 中注册 argv={"usertests", "case", 0}
→ 复用对应 Lab group
→ 通常不改 tests/run.py
```

### B. 已有独立测试程序

例如 `lazytests`、`cowtest`、`mmaptest`：确认源码位于 `tests/guest/`，成功 `exit(0)`，失败 `exit(non-zero)`，再注册到 `xv6test`。

如果历史程序只打印失败文本但最终仍 `exit(0)`，必须先修复退出状态，不能让 Python 用正则补救。

### C. 被测对象是 `user/` 下的用户程序

测试程序必须放在 `tests/guest/`，通过 `fork` / `exec` / pipe 黑盒测试被测程序：

```text
user/<program>.c                被测实现
        ↑ exec / pipe
 tests/guest/<program>test.c    测试场景和断言
```

不要把测试断言写回被测命令，也不要复制实现函数到测试文件。需要捕获 stdout/stderr 时优先复用 `tests/guest/testlib.h` 和 `tests/guest/testlib.c`。

### D. 内核控制台输出无法由 guest fd 捕获

例如 backtrace、trace 控制台行：guest 测试先验证操作成功并 `exit(0)`，`xv6test` 汇总 `status=0`，Python 只对同一 group 原始日志补充最小控制台观察。

Host 检查不得重新拥有完整业务语义。

### E. 测试会 panic、死锁或破坏磁盘状态

为该测试建立独立原子 suite，由 Python 启动单独 QEMU snapshot：

```text
xv6test --run <test-name>
```

不要与其他测试共用 QEMU。Lab9 `bigfile` 与 `symlinktest` 是现有示例。

### F. Host-only pthread 测试

例如 `ph`、`barrier`。测试断言写在 `tests/run.py`，被测 host 实现可以保留在 `notxv6/`。不要伪装成 xv6 guest 测试。

### G. 性能、竞争或调试观察

吞吐量、锁竞争计数、GDB 跟踪、概率时序不应直接成为稳定 `make test` 阻塞项，除非已有明确阈值、固定环境和重复性证据。优先建立独立实验或操作手册。

## 目录规则

测试源码必须放在：

```text
tests/guest/*.c
tests/guest/*.S
tests/guest/*.h
tests/guest/*.sh
tests/*.py
```

禁止放在：

```text
user/*test.c
kernel/*test.c
user/xv6test.c
```

被测实现继续位于 `user/`、`kernel/`、`notxv6/`。

Makefile 会把 `tests/guest/foo.c` 编译成 `user/_foo`。这是为了保持 `mkfs` 输入格式和 guest 命令名，不得因为源码分离而改变命令名。

## 新增 guest 测试标准流程

### 1. 设计中心行为

每个测试只验证一个紧密相关的行为集合，并写清前置状态、触发操作、可观察结果、关键不变量、清理路径和教学边界。

### 2. 编写测试源码

测试覆盖适用项：正常路径、边界输入、错误路径、资源释放、重复执行、并发场景和一个击穿错误直觉的反例。

成功：

```c
exit(0);
```

失败：

```c
printf("featuretest: reason\n");
exit(1);
```

参数错误建议 `exit(2)`。禁止“打印 FAILED 后仍 `exit(0)`”。

### 3. 接入 Makefile

普通测试源码：

1. 放入 `tests/guest/<name>.c`；
2. 将 `$U/_<name>` 加入 `UPROGS`。

通用规则会从 `$T/<name>.o` 链接到 `$U/_<name>`。测试需要额外对象时添加显式规则，例如：

```make
$U/_featuretest: $T/featuretest.o $T/testlib.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
```

不得把源文件复制回 `user/`。脚本若受 `mkfs` 路径限制，可像 `xargstest.sh` 一样在构建时生成 `user/` 临时副本，并在 `clean` 删除。

### 4. 注册 `xv6test`

```c
static char *lab8_feature_argv[] = {"usertests", "feature", 0};
```

```c
{"lab8", "lab8-feature", lab8_feature_argv},
```

命名规则：group 使用 `lab1` 到 `lab10`、`core`、`legacy`、`regression`；name 使用 `<group>-<behavior>`。名称必须全局唯一且稳定。

### 5. 判断是否修改 Python

已有 group 内新增普通测试：不修改 `tests/run.py`。

只有以下情况修改 Python：新增 group 宿主机入口、需要独立 QEMU snapshot、host-only 测试、guest 无法捕获的内核控制台输出、经过实测需要调整 timeout。

Python QEMU 命令必须通过：

```text
xv6test --group <group>
xv6test --run <test>
```

禁止重新添加直接执行 `usertests case` 并匹配业务输出的 `TestCase`。

### 6. 更新 runner 自测和文档

至少检查：suite 引用存在、QEMU Lab 命令全部经 `xv6test`、测试源码只位于 `tests/guest/`、新测试不与其他 suite 重复、特殊控制台检查仍要求 `XV6TEST done status=0`、README coverage map 已更新。

## 输出协议

```text
XV6TEST begin group=<group> test=<name>
XV6TEST run <n> - <name> group=<group>
XV6TEST ok <n> - <name>
XV6TEST not ok <n> - <name> ...
XV6TEST summary selected=<n> passed=<n> failed=<n>
XV6TEST done status=<0|1>
```

规则：空选择必须失败；每个测试独立 `fork` / `exec`；父进程准确回收目标 PID；任何非零退出状态成为 `not ok`；Python 只以 `XV6TEST done status=0` 判断 guest 汇总成功。

修改协议必须同步修改 `tests/guest/xv6test.c`、`tests/run.py`、`tests/test_runner.py`、`tests/README.md` 和本 Skill。

## 验证矩阵

必须运行：

```bash
make clean
make
make test-unit
make test CPUS=3
```

新增测试单项：

```bash
make qemu CPUS=3
```

在 xv6 shell：

```text
xv6test --list
xv6test --run <test-name>
xv6test --group <group>
xv6test --run <test-name>
```

单项至少重复运行两次。并发相关可补充 `make test CPUS=1`，但不得用单核通过证明多核正确。磁盘破坏型测试必须使用独立 snapshot。

无法运行时必须记录未运行命令、缺失环境、已完成静态检查和仍需用户执行的验收，禁止使用“应该通过”。

## GitHub 工作流

所有持久化改动必须有 Issue、从最新 `main` 创建或同步的分支、真实验证和 Draft PR，由用户手动合并。

PR 必须包含测试对象和中心行为、测试源码路径、xv6test group/name、snapshot 边界、正常/边界/错误/清理/重复执行证据、`CPUS=3` 与必要的 `CPUS=1` 结果、未运行项目和教学边界。

### GitHub Actions 禁令

本仓库测试任务不得新增或修改 `.github/workflows/*.yml` / `*.yaml`，不得依赖 GitHub Actions 作为唯一验证证据，不得私自把 Draft PR 标记为 ready。

## 禁止事项

- 禁止把测试源码放入 `user/` 或 `kernel/`；
- 禁止把被测实现复制到测试目录；
- 禁止把测试业务语义写成 Python 正则；
- 禁止空 group 假通过；
- 禁止失败后 `exit(0)`；
- 禁止不清理文件、进程、描述符或映射；
- 禁止为凑覆盖重复运行同一个底层用例；
- 禁止用单核证明多核；
- 禁止把性能实验伪装成稳定功能回归；
- 禁止虚构构建、QEMU 或测试结果；
- 禁止新增或修改 GitHub Actions workflow。

## 交付检查表

- [ ] 测试源码全部位于 `tests/`；
- [ ] `user/`、`kernel/` 不包含本轮测试源码；
- [ ] 被测实现未复制；
- [ ] 成功和失败退出状态可靠；
- [ ] 每个测试可单独运行；
- [ ] 已注册稳定 group/name；
- [ ] 已避免重复覆盖；
- [ ] 已检查资源清理和重复执行；
- [ ] Python 只处理基础设施或无法捕获的控制台观察；
- [ ] `make test-unit` 已通过；
- [ ] `make clean && make` 已真实运行或明确未运行；
- [ ] `make test CPUS=3` 已真实运行或明确未运行；
- [ ] 未新增或修改 GitHub Actions；
- [ ] Draft PR 已更新但未合并。
