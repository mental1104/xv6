# xv6 regression suites

本仓库使用 `tests/` 作为唯一测试根目录：

```text
tests/
├── guest/                 编译并在 xv6 内运行的 C/ASM/脚本测试源码
├── host/                  在宿主机 pthread 环境运行的测试源码
├── legacy/                保留的课程评分脚本与其支持库
├── run.py                 宿主机 QEMU 生命周期、超时、日志和 suite 编排
├── test_runner.py         不启动 QEMU 的 Python runner 自测
└── README.md
```

仓库根目录、`user/`、`kernel/` 与 `notxv6/` 不保存测试源码；旧课程评分入口位于 `tests/legacy/`。测试二进制仍生成到 `user/_<name>`，这是构建产物位置，不是源码所有权；这样可以保持 `mkfs` 输入格式和 xv6 shell 中的命令名不变。

## 责任边界

```text
被测实现：user/、kernel/
→ 提供真实用户程序、系统调用、内核机制和用户态库

测试语义：tests/guest/
→ 构造场景、执行断言、清理资源、通过 exit status 报告结果

测试注册：tests/guest/xv6test.c
→ 按 group/name 注册、fork/exec 隔离、wait 回收、输出 XV6TEST 协议

宿主机基础设施：tests/run.py
→ 启停 QEMU、设置 timeout、保存日志、隔离磁盘 snapshot、汇总 suite
```

核心原则：**行为断言属于 guest 测试，Python 不解析普通测试程序的自然语言输出。**

目前只有两类硬件/内核控制台输出需要 host 补充观察：

- Lab2 trace：确认 group 日志出现 `syscall read ->`；
- Lab4 backtrace：确认 group 日志至少出现三行返回地址。

这两个检查都建立在 `XV6TEST done status=0` 已成立的基础上，不替代 guest 的退出状态。

## 常用命令

旧课程 util grader 如需运行，入口改为：

```bash
python3 tests/legacy/grade-lab-util
```

```bash
# 默认入口：先验证 Python runner，再运行 Lab1-Lab10 与 focused usertests。
make test

# 只测试 Python runner 和仓库测试布局，不构建/启动 xv6。
make test-unit
make test-grader

# QEMU 集成范围。
make test-integration
make test-labs
make test-usertests
make test-full
make test-suite SUITE=lab-vm
python3 tests/run.py --list
```

进入 xv6 shell 后可以直接运行：

```text
xv6test --list
xv6test --group lab1
xv6test --group lab2
xv6test --group lab3
xv6test --group lab4
xv6test --group lab5
xv6test --group lab6
xv6test --group lab7
xv6test --group lab8
xv6test --group lab9
xv6test --group lab10
xv6test --group core
xv6test --run lab9-bigfile
xv6test --run usertests-full
```

成功调用必须以以下协议结束：

```text
XV6TEST summary selected=<n> passed=<n> failed=0
XV6TEST done status=0
```

未知 group/name 或空选择必须失败，避免拼写错误产生假阳性。每个注册测试都在独立子进程执行，`wait()` 将目标测试程序的退出状态传播到 group 汇总。

## Lab1-Lab10 映射

| Lab | guest 入口 | 主要测试源码 | host 补充检查 |
|---|---|---|---|
| Lab1 utilities | `xv6test --group lab1` | `tests/guest/lab1test.c` | 无 |
| Lab2 syscall | `xv6test --group lab2` | `tracemasktest.c`、`sysinfotest.c`、`tracesmoke.c` | trace 控制台行 |
| Lab3 page tables | `xv6test --group lab3` | `usertests.c` 指定用例 | 无 |
| Lab4 traps | `xv6test --group lab4` | `bttest.c`、`alarmtest.c` | backtrace 地址行 |
| Lab5 lazy allocation | `xv6test --group lab5` | `lazytests.c` | 无 |
| Lab6 COW | `xv6test --group lab6` | `cowtest.c` | 无 |
| Lab7 threads | `xv6test --group lab7` | `guest/uthreadtest.c`；`host/ph.c`、`host/barrier.c` | `ph`、`barrier` 由 Python 在宿主机执行 |
| Lab8 locks | `xv6test --group lab8` | `sbrkmuch`、`createdelete`、`fourfiles`、`bigwrite` | 无不稳定性能阈值 |
| Lab9 file system | `--run lab9-bigfile` / `--run lab9-symlink` | `bigfile.c`、`symlinktest.c` | 分开 QEMU snapshot |
| Lab10 mmap | `xv6test --group lab10` | `mmaptest.c` | 无 |

`sbrkmuch` 同时属于 Lab3 地址空间增长和 Lab8 allocator 行为，因此按两个稳定测试名注册；这是有意保留的跨 Lab 共享回归。

Lab9 的两个测试虽然都属于 `lab9` group，但默认宿主机 suite 使用两次 `--run` 并分别启动 snapshot。`bigfile` 会显著修改文件系统，不应与 `symlinktest` 共用自动化 snapshot。

## 新增测试的最短路径

1. guest 测试放入 `tests/guest/`，host-only 测试放入 `tests/host/`；不要放入 `user/`、`kernel/` 或 `notxv6/`。
2. 测试通过必须 `exit(0)`，断言或资源清理失败必须 `exit(non-zero)`。
3. 被测对象通过系统调用、`exec()`、公开头文件或可链接用户态对象访问，不要复制实现。
4. 将生成命令加入 `Makefile::UPROGS`；普通测试可由 `$U/_%: $T/%.o` 自动链接。
5. 在 `tests/guest/xv6test.c` 注册唯一名称、group 和 argv。
6. 已有 group 时通常不用修改 `tests/run.py`；新增 snapshot 边界或 host-only 测试时才改。
7. 执行：

```bash
make clean
make
make test-unit
make test CPUS=3
```

需要诊断并发问题时可以补充 `CPUS=1`，但单核结果不能证明多核正确。

完整 AI/开发者工作流见：

```text
.codex/skills/xv6-guest-first-testing/SKILL.md
```

## 日志与 snapshot

每个原子 QEMU suite 从基础 `fs.img` 以 `-snapshot` 启动，写入在实例结束后丢弃。原始输出保存在：

```text
test-results/<suite>/<test>.log
```

测试失败时首先查看对应日志；不要只依据终端最后一行判断。
