---
name: xv6-guest-first-testing
description: 在 mental1104/xv6 中新增、迁移、整理或维护测试时使用。适用于“新增测试”“补测试”“迁移到 xv6test”“调整 make test”“修改 tests/run.py”“为某个 Lab 增加回归”等请求。要求优先让 xv6 guest 持有测试语义，Python 只负责 QEMU 编排、超时和日志；禁止新增或修改 GitHub Actions workflow；所有持久化改动必须通过 Issue、分支和 Draft PR。
---

# xv6 Guest-first 测试最佳实践

## 目标

在本仓库中新增测试时，保持以下责任边界：

```text
xv6 用户态测试程序
├── 构造输入与场景
├── 执行断言
├── 覆盖正常、边界和错误路径
└── 通过 exit status 报告成功或失败

user/xv6test.c
├── 注册测试名称与 group
├── fork / exec 隔离每个测试
├── wait 回收并传播退出状态
└── 输出稳定 XV6TEST 协议

宿主机 tests/run.py
├── 启动和停止 QEMU
├── 设置超时
├── 保存原始日志
├── 调用 guest group
└── 只判断稳定结束协议
```

核心原则：

> 测试语义留在 guest；Python 只拥有基础设施语义。

不要把新的 xv6 行为断言继续堆进 Python 正则。

---

## 触发条件

处理以下请求时必须使用本 Skill：

- 新增 xv6 测试；
- 给某个 Lab 补回归测试；
- 把已有 `usertests`、`lazytests`、`cowtest` 等迁移到 `xv6test`；
- 调整 `make test`、`tests/run.py`、`tests/test_runner.py`；
- 为新用户程序或新内核机制设计自动验收；
- 修复“测试明明失败但 runner 判定成功”；
- 整理或扩展 guest-first 测试框架。

---

## 开始前必须检查

AI 开工前必须完成：

1. 读取仓库根目录和目标目录中的：
   - `AGENTS.md`；
   - `CONTRIBUTING.md`；
   - `README`；
   - 现有测试说明与代码风格。
2. 读取并遵守强制代码注释 Skill。
3. 记录最新 `main` Commit SHA。
4. 检查实际文件，不能按原版 xv6 或教材记忆猜测：
   - `Makefile`；
   - `user/xv6test.c`；
   - 目标测试程序；
   - `tests/run.py`；
   - `tests/test_runner.py`；
   - `tests/README.md`。
5. 搜索是否已有相同或近似测试，避免重复覆盖。
6. 检查目标测试程序的所有成功和失败路径是否可靠调用 `exit(0)` / `exit(non-zero)`。
7. 建立或复用聚焦 Issue，从最新 `main` 创建分支，最终提交 Draft PR。

如果仓库不存在某个规范文件，应记录“未找到”，不能虚构内容。

---

## 先判断测试属于哪一类

### A. 已有 guest 程序，退出状态可靠

例如：

- `usertests <case>`；
- `cowtest`；
- 能正确返回状态的独立用户程序。

做法：

```text
直接注册到 user/xv6test.c
→ 放入合适 group
→ 检查是否需要从 Python 重复覆盖中移除
→ 更新 runner 组合测试和文档
```

这是优先路线。

### B. 已有 guest 程序，但只打印 PASS/FAIL

先检查能否在不改变测试语义的前提下修正退出状态。

例如：

```c
if(failed)
  exit(1);
exit(0);
```

要求：

- 每条失败路径最终必须返回非零；
- 不能只有打印 `FAILED` 而仍 `exit(0)`；
- 不能无论结果如何固定返回同一个状态；
- 修复后仍要保留对人可读的诊断输出。

如果无法可靠改造退出状态，则暂时留在 host-owned 输出规则，不能强行迁移。

### C. 尚无合适测试程序

新增最小 xv6 用户态测试程序：

```text
user/<feature>test.c
```

要求：

- 一个程序围绕一个紧密相关能力；
- 通过系统调用或用户可观察行为验证内核；
- 成功 `exit(0)`，失败 `exit(1)`；
- 加入 `Makefile::UPROGS`；
- 再注册进 `xv6test`。

不要为了测试而把大量断言放进内核生产路径。

### D. 测试可能 panic、死锁或破坏整个 guest

这类测试无法依靠单个 child process 隔离。

例如：

- 故意触发 kernel panic；
- 可能卡死所有 CPU；
- 会破坏文件系统状态；
- 需要不同内核编译配置。

做法：

```text
单独 QEMU snapshot / atomic suite
→ 独立超时
→ 独立日志
→ 不与普通 group 混跑
```

必要时保留在 Python 编排层，但测试语义仍应尽量由 guest 输出稳定终止标记。

### E. 宿主机 pthread 或工具测试

例如 `notxv6/ph.c`、`notxv6/barrier.c`。

继续由 Python host runner 执行，不注册到 `xv6test`。

### F. 性能、锁竞争或调试观察

默认视为实验，不直接作为稳定阻塞回归。

除非已有明确阈值、稳定环境和重复证据，否则：

- 不使用共享 runner 上的脆弱倍率；
- 不用一次 wall-clock 结果判定正确性；
- 单独建立实验程序和证据记录；
- 功能正确性与性能实验分开。

---

## 用户新增测试的最短路径

### 情况 1：注册一个已有 `usertests` 用例

在 `user/xv6test.c` 中增加 argv：

```c
static char *lab8_createdelete_argv[] = {"usertests", "createdelete", 0};
```

再加入注册表：

```c
{"lab8", "lab8-createdelete", lab8_createdelete_argv},
```

如果 `tests/run.py` 已经调用：

```text
xv6test --group lab8
```

通常不需要再修改 Python。

### 情况 2：注册一个已有独立程序

```c
static char *lab10_mmaptest_argv[] = {"mmaptest", 0};
```

```c
{"lab10", "lab10-mmaptest", lab10_mmaptest_argv},
```

注册前必须确认 `mmaptest`：

- 成功时返回 0；
- 任一失败时返回非零；
- 不会在成功输出后错误返回失败；
- 不依赖外部 Python 正则才能判断结果。

### 情况 3：新增一个用户测试程序

先写测试程序并加入 `UPROGS`：

```make
UPROGS=\
	...\
	$U/_featuretest
```

测试程序最小结构：

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/**
 * 验证目标行为，并通过退出状态报告结果。
 *
 * @return 成功时通过 exit(0) 结束，失败时通过 exit(1) 结束。
 */
int
main(int argc, char *argv[])
{
  int failed = 0;

  // 正常路径。
  if(/* 观察结果不符合预期 */){
    printf("featuretest: normal path failed\n");
    failed = 1;
  }

  // 边界或错误路径。
  if(/* 错误输入没有被拒绝 */){
    printf("featuretest: invalid input accepted\n");
    failed = 1;
  }

  exit(failed);
}
```

然后将 `featuretest` 注册进 `xv6test`。

---

## 命名规范

### Group

优先使用稳定能力或 Lab 名：

```text
lab3
lab5
lab6
lab8
filesystem
scheduler
```

同一类测试应聚合到同一 group，不要为单个测试随意创建 group。

### Test name

格式：

```text
<group>-<behavior>
```

示例：

```text
lab3-copyin
lab5-lazytests
lab8-createdelete
filesystem-log-recovery
```

要求：

- 全局唯一；
- 稳定，不包含动态 PID、地址或时间；
- 描述被验证的行为，不使用 `test1`、`misc`、`new-test`。

### argv 变量

格式：

```text
<group>_<behavior>_argv
```

示例：

```c
static char *lab8_createdelete_argv[] = {"usertests", "createdelete", 0};
```

---

## `xv6test` 注册约束

每个注册项必须满足：

- `group` 非空；
- `name` 全局唯一；
- `argv[0]` 是 `fs.img` 内实际存在的程序；
- `argv` 以 `0` 结尾；
- 目标程序退出状态可靠；
- 测试之间不依赖执行顺序；
- 测试失败不会被下一项覆盖；
- 测试完成后不会留下未回收直接子进程。

`xv6test` 的稳定协议包括：

```text
XV6TEST begin ...
XV6TEST run ...
XV6TEST ok ...
XV6TEST not ok ...
XV6TEST summary ...
XV6TEST done status=<0|1>
```

宿主机只应把以下行作为 group 成功依据：

```text
XV6TEST done status=0
```

不要让 Python 匹配新 guest 测试内部的自然语言输出。

---

## 什么时候修改 `tests/run.py`

### 不需要修改

向一个已经被 host runner 调用的 group 增加测试时：

```text
只改 guest 注册表和目标测试程序
```

例如 `lab3` 已经由：

```python
("xv6test --group lab3",)
```

接入 Python，则新增 `lab3` 注册项通常无需修改 `tests/run.py`。

### 需要修改

以下情况才修改：

- 新增一个尚未接入的 group；
- 新增必须独立 QEMU snapshot 的 atomic suite；
- 调整合理的 group timeout；
- 移除已迁移测试的重复 host 规则；
- host-only 测试确实需要新增命令。

新增 guest group 时应使用：

```python
TestCase(
    "lab8-guest-tests",
    ("xv6test --group lab8",),
    expected=GUEST_SUCCESS,
    timeout=300,
)
```

禁止：

```python
expected=(r"createdelete: OK", r"fourfiles: OK")
```

因为这会重新把测试语义拉回 Python。

---

## `tests/test_runner.py` 必须补什么

修改 host suite 时至少覆盖：

1. 新 group 确实通过 `xv6test --group ...` 调用；
2. 不再保留已经迁移的旧命令；
3. `GUEST_SUCCESS` 必须要求 `status=0`；
4. suite include 引用有效；
5. 同一个 atomic suite 内测试名称不重复；
6. 已迁移测试不会在 `usertests-core` 等地方重复执行。

不要用 runner 单元测试假装验证了 xv6 内核行为。它只验证编排和协议。

---

## 测试用例设计要求

一个新增功能至少考虑：

- 正常路径；
- 边界输入；
- 错误路径；
- 资源释放；
- 重复执行；
- 必要的并发场景；
- 一个击穿错误直觉的反例。

并非每个小改动都必须机械新增七个 case，但必须明确哪些维度适用、哪些不适用。

### 资源释放

按目标能力检查：

- 进程是否被 `wait()` 回收；
- 文件描述符是否关闭；
- 文件或目录是否清理；
- 页是否归还；
- 锁是否释放；
- 重复执行后是否出现泄漏或状态残留。

### 并发

- 默认多核验证使用 `CPUS=3`；
- `CPUS=1` 只能作为补充诊断；
- 不得用单核结果证明多核正确；
- 禁止依赖随机调度才能命中；
- 使用 pipe、wait、显式状态等方式构造确定性同步。

### 时序

避免：

- 依赖宿主机速度；
- 依赖精确 sleep 时长；
- 使用随机数碰撞竞态；
- 使用不稳定性能倍率；
- 只因“多跑几次没出错”就判定并发正确。

---

## 本地验证顺序

### 最小静态检查

```bash
python3 -m py_compile tests/run.py tests/test_runner.py
make test-unit
python3 tests/run.py --list
```

### 构建

```bash
make clean
make
```

不得把 host `gcc -fsyntax-only` 等同于真实 RISC-V 构建。

### 单项 guest 验证

```bash
make qemu
```

在 xv6 shell 中：

```text
xv6test --list
xv6test --run <test-name>
xv6test --group <group>
xv6test --group <group>
```

同一 group 连续运行两次，用于暴露资源泄漏和残留状态。

### 完整入口

```bash
make test CPUS=3
```

必要时补充：

```bash
make test-suite SUITE=<suite> CPUS=3
make test CPUS=1
```

单核只用于定位问题。

### 检查 GitHub Actions 边界

```bash
git diff --name-only main...HEAD
```

本仓库测试改动不得新增或修改：

```text
.github/workflows/*.yml
.github/workflows/*.yaml
```

除非用户在另一轮请求中明确批准。

---

## AI 执行规则

### 必须做

- 先读实际仓库，不按上游 xv6 猜测；
- 固定并报告基线 Commit；
- 优先复用已有测试程序；
- 检查完整退出状态路径；
- 使用 guest group，而不是新增业务输出正则；
- 避免重复回归；
- 添加必要的 runner 组合测试；
- 运行真实可用的构建和测试；
- 对未运行项目明确写“未运行”及原因；
- 更新 Issue、分支和 Draft PR；
- 在 PR 中记录用户本地可执行的逐条验收命令；
- 保持普通 `make`、`make qemu` 和现有 ABI 不变，除非目标功能本身要求改变。

### 禁止做

- 禁止直接提交 `main`；
- 禁止无 Issue 开工；
- 禁止新增或修改 GitHub Actions workflow；
- 禁止用 Python 正则重新承载已迁移 guest 测试的语义；
- 禁止注册退出状态不可靠的程序；
- 禁止空 group 假通过；
- 禁止重复执行相同测试却不说明；
- 禁止用打印文本代替退出状态；
- 禁止把 runner 单元测试描述为内核行为测试；
- 禁止把 host 语法检查描述为真实交叉编译通过；
- 禁止把未运行的 QEMU 场景写成通过；
- 禁止以 `CPUS=1` 证明并发正确；
- 禁止依赖公网、随机时序或共享 runner 性能作为稳定正确性门禁。

---

## 迁移已有测试时的检查表

- [ ] 找到现有测试程序和入口；
- [ ] 检查所有成功路径返回 0；
- [ ] 检查所有失败路径返回非零；
- [ ] 检查目标程序存在于 `UPROGS`；
- [ ] 添加唯一 `name`、合理 `group` 和 NUL 结尾 argv；
- [ ] `xv6test --list` 能列出测试；
- [ ] `xv6test --run <name>` 能独立执行；
- [ ] `xv6test --group <group>` 能正确汇总；
- [ ] Python 只匹配 `XV6TEST done status=0`；
- [ ] 删除旧的重复 host 规则；
- [ ] 更新 `tests/test_runner.py`；
- [ ] 更新 `tests/README.md`；
- [ ] 连续执行两次无残留；
- [ ] 多核场景已验证或明确标记未运行；
- [ ] diff 未触碰 `.github/workflows/`。

---

## 新增测试时的交付汇报

```markdown
## 完成情况
- 目标能力：
- 测试类型：已有 guest / 新用户程序 / 独立 QEMU / host-only / 实验
- group：
- test name：
- 基线 Commit：

## 测试语义
- 正常路径：
- 边界路径：
- 错误路径：
- 资源释放：
- 并发与时序：
- 反例：

## 接入路径
- guest 程序：
- `xv6test` 注册：
- `tests/run.py`：
- 重复覆盖处理：

## 验证结果
- `make clean && make`：
- `make test-unit`：
- 单项 guest：
- group guest：
- `make test CPUS=3`：
- `CPUS=1` 补充：
- 未运行项目：

## 工作项
- Issue：
- 分支：
- Draft PR：
- GitHub Actions workflow：未新增、未修改
```

---

## 当前架构入口

以仓库实际代码为准，通常需要检查：

```text
Makefile
user/xv6test.c
tests/run.py
tests/test_runner.py
tests/README.md
```

当前 `make test` 的预期主线：

```text
make test
→ test-unit
→ test-integration
→ tests/run.py
→ QEMU snapshot
→ xv6test --group ...
→ XV6TEST done status=0
```

如果实际代码与本 Skill 不一致，应先调查差异，更新 Skill 或实现，不能静默按旧说明执行。
