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
FAILURE_EXCERPT_LINES = 80
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


def _commands(selected_cases: tuple[str, ...] = ()) -> tuple[str, ...]:
    """生成压力前序、完整逐项或用户指定的 xv6test 命令。

    Args:
        selected_cases: 需要定向执行的 usertests 名称。非空时保持传入顺序且只运行
            这些用例，不额外加入默认压力前序；空元组表示执行完整回归。

    Returns:
        可依次发送到同一 xv6 Shell 的命令元组。
    """

    if selected_cases:
        return tuple(f"xv6test --usertest {name}" for name in selected_cases)

    commands: list[str] = []
    for name, count in STRESS_CASES:
        commands.extend(f"xv6test --usertest {name}" for _ in range(count))
    commands.extend(f"xv6test --usertest {name}" for name in USERTEST_CASES)
    return tuple(commands)


def _assert_command_output(command: str, output: str) -> None:
    """验证单个 usertests 子项经过统一协议成功结束。

    Args:
        command: 发送到 xv6 Shell 的完整命令，用于形成可定位的失败信息。
        output: 从命令回显开始到下一个 Shell prompt 之前的原始输出。

    Raises:
        RUNNER.TestFailure: completion 状态非零、缺失协议标记、缺失业务成功文本，
            或输出匹配默认拒绝模式时抛出。
    """

    normalized = RUNNER._normalize_output(output)
    completion = RUNNER.re.search(
        r"^XV6TEST done status=(-?\d+)$",
        normalized,
        RUNNER.re.MULTILINE,
    )
    if completion is None:
        raise RUNNER.TestFailure(f"{command}: missing XV6TEST completion marker")

    status = int(completion.group(1))
    if status != 0:
        raise RUNNER.TestFailure(f"{command}: XV6TEST completed with status={status}")
    if "ALL TESTS PASSED" not in normalized:
        raise RUNNER.TestFailure(f"{command}: usertests did not report success")
    for pattern in RUNNER.DEFAULT_REJECTED:
        if RUNNER.re.search(pattern, normalized, RUNNER.re.MULTILINE):
            raise RUNNER.TestFailure(f"{command}: matched rejected pattern: {pattern}")


def _format_failure_output(command: str, output: str) -> str:
    """把当前失败命令的 guest 输出整理为适合 CI 终端展示的有界片段。

    Args:
        command: 产生失败输出的 xv6 Shell 命令。
        output: 从该命令回显后到 Shell prompt 或失败点之前捕获的原始输出。

    Returns:
        带开始、结束标记的文本片段。最多保留最后 FAILURE_EXCERPT_LINES 行；
        完整输出仍由调用者写入 test-results 日志，避免完整回归失败时淹没 CI 页面。
    """

    normalized = RUNNER._normalize_output(output).rstrip("\n")
    lines = normalized.splitlines() if normalized else []
    omitted = max(0, len(lines) - FAILURE_EXCERPT_LINES)
    visible = lines[-FAILURE_EXCERPT_LINES:]

    excerpt = [f"--- failing guest output: {command} ---"]
    if omitted:
        excerpt.append(f"... {omitted} earlier lines omitted ...")
    excerpt.extend(visible or ["<no guest output captured>"])
    excerpt.append("--- end failing guest output ---")
    return "\n".join(excerpt)


def run(cpus: int, selected_cases: tuple[str, ...] = ()) -> None:
    """在同一 QEMU snapshot 中逐项运行并立即保存失败现场。

    Args:
        cpus: QEMU 暴露给 xv6 的 CPU 数量，必须大于 0。
        selected_cases: 可选定向用例；为空时执行默认完整回归。

    Raises:
        RUNNER.TestFailure: QEMU、guest completion 协议或输出约束失败时抛出；
            所有路径都会先把当前累计输出写入 usertests-full 日志，并把当前失败
            命令的有界输出附在异常中供 CI 直接展示。
    """

    child = RUNNER._start_qemu(cpus)
    chunks = [child.before]
    try:
        for command in _commands(selected_cases):
            child.timeout = WATCHDOG_SECONDS
            child.sendline(command)
            result = RUNNER._wait_for_qemu_command(child)
            chunks.append(f"$ {command}\n{result.output}")
            output = "\n".join(chunks)
            if result.failure is not None:
                log_path = RUNNER._write_log("usertests-full", "usertests-full", output)
                excerpt = _format_failure_output(command, result.output)
                raise RUNNER.TestFailure(
                    f"{result.failure}\n{excerpt}\nfull log: {log_path}"
                )
            try:
                _assert_command_output(command, result.output)
            except RUNNER.TestFailure as exc:
                # 输出断言失败同样属于需要保留的 guest 现场，不能只在 panic/timeout
                # 路径写日志，否则真正的 status=1 和业务失败文本会被调用者丢失。
                log_path = RUNNER._write_log("usertests-full", "usertests-full", output)
                excerpt = _format_failure_output(command, result.output)
                raise RUNNER.TestFailure(
                    f"{exc}\n{excerpt}\nfull log: {log_path}"
                ) from exc

        output = "\n".join(chunks)
        log_path = RUNNER._write_log("usertests-full", "usertests-full", output)
        print(f"PASS usertests-full ({log_path.relative_to(REPO_ROOT)})")
    finally:
        RUNNER._stop_qemu(child)


def parse_args() -> argparse.Namespace:
    """解析 QEMU CPU 数量和可重复的定向 usertests 名称。"""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, default=3, help="QEMU CPU count")
    parser.add_argument(
        "--case",
        action="append",
        choices=USERTEST_CASES,
        dest="cases",
        help="run only this usertests case; may be supplied more than once",
    )
    return parser.parse_args()


def main() -> int:
    """执行请求的完整或定向回归并返回稳定退出状态。"""

    args = parse_args()
    if args.cpus < 1:
        raise SystemExit("--cpus must be at least 1")
    try:
        run(args.cpus, tuple(args.cases or ()))
    except RUNNER.TestFailure as exc:
        # 失败信息写 stderr；GitHub Actions 会与 stdout 一同显示在当前 step 日志中。
        print(f"FAIL usertests-full: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
