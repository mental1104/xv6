#!/usr/bin/env python3
"""逐项运行 usertests，并为每一项设置五分钟无进展看门狗。"""

from __future__ import annotations

import argparse
import importlib.util
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
RUNNER_PATH = Path(__file__).with_name("run.py")
SPEC = importlib.util.spec_from_file_location("xv6_regression_runner", RUNNER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load regression runner from {RUNNER_PATH}")
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)

WATCHDOG_SECONDS = 300
JOBCTL_STRESS_RUNS = 8
FORK_STRESS_RUNS = 8

# writebig 与独立 lab9-bigfile 都验证 MAXFILE 个块的完整写入和读回；后者还断言
# MAXFILE + 1 边界必须失败，因此 CI 只保留覆盖更强且已有独立 snapshot 的
# lab9-bigfile，避免在 usertests 中重复执行同一条 65,803 块慢路径。
USERTEST_CASES = (
    "execout", "copyin", "copyout", "copyinstr1", "copyinstr2", "copyinstr3",
    "truncate1", "truncate2", "truncate3", "reparent2", "jobctl", "pgbug",
    "sbrkbugs", "badarg", "reparent", "twochildren", "forkfork",
    "forkforkfork", "argptest", "createdelete", "linkunlink", "linktest",
    "unlinkread", "concreate", "subdir", "fourfiles", "sharedfd", "exectest",
    "bigargtest", "bigwrite", "bsstest", "sbrkbasic", "sbrkmuch", "kernmem",
    "sbrkfail", "sbrkarg", "validatetest", "stacktest", "opentest", "writetest",
    "createtest", "openiput", "exitiput", "iput", "mem", "pipe1", "preempt",
    "exitwait", "rmdot", "fourteen", "bigfile", "dirfile", "iref", "forktest",
    "bigdir",
)

STRESS_CASES = (
    ("reparent2", 1),
    ("jobctl", JOBCTL_STRESS_RUNS),
    ("forkforkfork", FORK_STRESS_RUNS),
)


def _commands() -> tuple[str, ...]:
    """按压力前序和完整逐项回归顺序生成 xv6test 命令。"""

    commands: list[str] = []
    for name, count in STRESS_CASES:
        commands.extend(f"xv6test --usertest {name}" for _ in range(count))
    commands.extend(f"xv6test --usertest {name}" for name in USERTEST_CASES)
    return tuple(commands)


def _assert_command_output(command: str, output: str) -> None:
    """验证单个 usertests 子项经过统一协议成功结束。"""

    normalized = RUNNER._normalize_output(output)
    if "XV6TEST done status=0" not in normalized:
        raise RUNNER.TestFailure(f"{command}: missing successful XV6TEST completion")
    if "ALL TESTS PASSED" not in normalized:
        raise RUNNER.TestFailure(f"{command}: usertests did not report success")
    for pattern in RUNNER.DEFAULT_REJECTED:
        if RUNNER.re.search(pattern, normalized, RUNNER.re.MULTILINE):
            raise RUNNER.TestFailure(f"{command}: matched rejected pattern: {pattern}")


def run(cpus: int) -> None:
    """在同一 QEMU snapshot 中逐项运行并立即保存失败现场。"""

    child = RUNNER._start_qemu(cpus)
    chunks = [child.before]
    try:
        for command in _commands():
            child.timeout = WATCHDOG_SECONDS
            child.sendline(command)
            result = RUNNER._wait_for_qemu_command(child)
            chunks.append(f"$ {command}\n{result.output}")
            output = "\n".join(chunks)
            if result.failure is not None:
                log_path = RUNNER._write_log("usertests-full", "usertests-full", output)
                raise RUNNER.TestFailure(f"{result.failure}; log: {log_path}")
            _assert_command_output(command, result.output)

        output = "\n".join(chunks)
        log_path = RUNNER._write_log("usertests-full", "usertests-full", output)
        print(f"PASS usertests-full ({log_path.relative_to(REPO_ROOT)})")
    finally:
        RUNNER._stop_qemu(child)


def parse_args() -> argparse.Namespace:
    """解析 QEMU CPU 数量。"""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, default=3, help="QEMU CPU count")
    return parser.parse_args()


def main() -> int:
    """执行逐项回归并返回稳定退出状态。"""

    args = parse_args()
    if args.cpus < 1:
        raise SystemExit("--cpus must be at least 1")
    try:
        run(args.cpus)
    except RUNNER.TestFailure as exc:
        print(f"FAIL usertests-full: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
