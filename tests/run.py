#!/usr/bin/env python3
"""通过 QEMU 或宿主机执行稳定的 xv6 回归测试套件。"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

import pexpect


REPO_ROOT = Path(__file__).resolve().parents[1]
RESULT_ROOT = REPO_ROOT / "test-results"
QEMU_SHELL_PROMPT = "$ "
QEMU_FATAL_OUTPUTS = (
    "panic:",
    "kerneltrap",
)
DEFAULT_REJECTED = QEMU_FATAL_OUTPUTS + (
    r"exec .* failed",
    r"\bFAIL(?:ED)?\b",
)
# guest-first 测试只向宿主机暴露稳定结束协议，不再匹配各测试程序的业务输出。
GUEST_SUCCESS = (r"^XV6TEST done status=0$",)


@dataclass(frozen=True)
class CountExpectation:
    """描述输出模式必须出现的最小次数。"""

    pattern: str
    minimum: int


@dataclass(frozen=True)
class TestCase:
    """描述一个 host 或 QEMU 测试及其基础设施级输出约束。"""

    name: str
    commands: tuple[str, ...]
    expected: tuple[str, ...] = ()
    counted: tuple[CountExpectation, ...] = ()
    rejected: tuple[str, ...] = DEFAULT_REJECTED
    timeout: int = 90
    host: bool = False


@dataclass(frozen=True)
class Suite:
    """描述一个原子测试套件或由其他套件组成的聚合套件。"""

    name: str
    tests: tuple[TestCase, ...] = ()
    includes: tuple[str, ...] = ()
    description: str = ""


@dataclass(frozen=True)
class QemuCommandResult:
    """记录一次 guest 命令等待 shell prompt 的输出和失败原因。"""

    output: str
    failure: str | None = None


SUITES: dict[str, Suite] = {
    "lab-basic": Suite(
        name="lab-basic",
        description="Lab1 utilities and Lab2 system calls through guest groups",
        tests=(
            TestCase(
                "lab1-guest-tests",
                ("xv6test --group lab1",),
                expected=GUEST_SUCCESS,
                timeout=240,
            ),
            TestCase(
                "lab2-guest-tests",
                ("xv6test --group lab2",),
                expected=GUEST_SUCCESS + (r"syscall read ->",),
                timeout=300,
            ),
        ),
    ),
    "lab-vm": Suite(
        name="lab-vm",
        description="Lab3-Lab6 page-table, trap, lazy allocation and COW groups",
        tests=(
            TestCase(
                "lab3-guest-tests",
                ("xv6test --group lab3",),
                expected=GUEST_SUCCESS,
                timeout=600,
            ),
            TestCase(
                "lab4-guest-tests",
                ("xv6test --group lab4",),
                expected=GUEST_SUCCESS,
                counted=(CountExpectation(r"^0x[0-9a-f]+$", 3),),
                timeout=300,
            ),
            TestCase(
                "lab5-guest-tests",
                ("xv6test --group lab5",),
                expected=GUEST_SUCCESS,
                timeout=300,
            ),
            TestCase(
                "lab6-guest-tests",
                ("xv6test --group lab6",),
                expected=GUEST_SUCCESS,
                timeout=360,
            ),
        ),
    ),
    "lab7-thread": Suite(
        name="lab7-thread",
        description="Lab7 guest uthread regression and host pthread exercises",
        tests=(
            TestCase(
                "lab7-guest-tests",
                ("xv6test --group lab7",),
                expected=GUEST_SUCCESS,
                timeout=240,
            ),
            TestCase(
                "lab7-ph-correctness",
                ("make ph", "./ph 2"),
                counted=(CountExpectation(r"^\d+: 0 keys missing$", 2),),
                host=True,
            ),
            TestCase(
                "lab7-barrier",
                ("make barrier", "./barrier 2"),
                expected=(r"^OK; passed$",),
                host=True,
            ),
        ),
    ),
    "lab8-locks": Suite(
        name="lab8-locks",
        description="Fast Lab8 buffer-cache guest regression",
        tests=(
            TestCase(
                "lab8-fast-guest-tests",
                (
                    "xv6test --run lab8-createdelete",
                    "xv6test --run lab8-fourfiles",
                ),
                expected=GUEST_SUCCESS,
                timeout=180,
            ),
        ),
    ),
    "lab9-bigfile": Suite(
        name="lab9-bigfile",
        description="Lab9 large-file test in an isolated disk snapshot",
        tests=(
            TestCase(
                "lab9-bigfile",
                ("xv6test --run lab9-bigfile",),
                expected=GUEST_SUCCESS,
                timeout=120,
            ),
        ),
    ),
    "lab9-symlink": Suite(
        name="lab9-symlink",
        description="Lab9 symbolic-link test in an isolated disk snapshot",
        tests=(
            TestCase(
                "lab9-symlink",
                ("xv6test --run lab9-symlink",),
                expected=GUEST_SUCCESS,
                timeout=240,
            ),
        ),
    ),
    "lab10-mmap": Suite(
        name="lab10-mmap",
        description="Lab10 mmap guest regression",
        tests=(
            TestCase(
                "lab10-guest-tests",
                ("xv6test --group lab10",),
                expected=GUEST_SUCCESS,
                timeout=420,
            ),
        ),
    ),
    "usertests-core": Suite(
        name="usertests-core",
        description="Focused cross-lab usertests exposed by the guest registry",
        tests=(
            TestCase(
                "usertests-core",
                ("xv6test --group core",),
                expected=GUEST_SUCCESS,
                timeout=900,
            ),
        ),
    ),
    "usertests-full": Suite(
        name="usertests-full",
        description="Complete xv6 usertests regression through xv6test",
        tests=(
            TestCase(
                "usertests-full",
                ("xv6test --run usertests-full",),
                expected=GUEST_SUCCESS,
                timeout=1200,
            ),
        ),
    ),
    "lab-concurrency": Suite(
        name="lab-concurrency",
        includes=("lab7-thread", "lab8-locks"),
    ),
    "lab-storage": Suite(
        name="lab-storage",
        includes=("lab9-bigfile", "lab9-symlink", "lab10-mmap"),
    ),
    "pr": Suite(
        name="pr",
        includes=(
            "lab-basic",
            "lab-vm",
            "lab7-thread",
            "lab8-locks",
            "lab9-bigfile",
            "lab9-symlink",
            "lab10-mmap",
            "usertests-core",
        ),
    ),
    "full": Suite(
        name="full",
        includes=(
            "lab-basic",
            "lab-vm",
            "lab7-thread",
            "lab8-locks",
            "lab9-bigfile",
            "lab9-symlink",
            "lab10-mmap",
            "usertests-full",
        ),
    ),
}


class TestFailure(RuntimeError):
    """表示测试基础设施或输出约束未满足。"""


def _safe_name(value: str) -> str:
    """将 suite/test 名称规范化为安全的日志文件名。"""

    return re.sub(r"[^A-Za-z0-9_.-]+", "-", value)


def _normalize_output(output: str) -> str:
    """统一终端行尾供正则匹配；原始日志仍由调用者原样保存。"""

    return output.replace("\r\n", "\n").replace("\r", "\n")


def _assert_output(test: TestCase, output: str) -> None:
    """检查拒绝、必需和计数模式，不解释 guest 业务语义。"""

    normalized = _normalize_output(output)
    flags = re.MULTILINE
    for pattern in test.rejected:
        if re.search(pattern, normalized, flags):
            raise TestFailure(f"matched rejected pattern: {pattern}")
    for pattern in test.expected:
        if not re.search(pattern, normalized, flags):
            raise TestFailure(f"missing expected pattern: {pattern}")
    for expectation in test.counted:
        count = len(re.findall(expectation.pattern, normalized, flags))
        if count < expectation.minimum:
            raise TestFailure(
                f"pattern {expectation.pattern!r} matched {count} times; "
                f"expected at least {expectation.minimum}"
            )


def _write_log(suite: str, test: str, output: str) -> Path:
    """保存原始测试输出并返回相对仓库可定位的日志路径。"""

    directory = RESULT_ROOT / _safe_name(suite)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{_safe_name(test)}.log"
    path.write_text(output, encoding="utf-8", errors="replace")
    return path


def _run_host_test(suite: str, test: TestCase) -> None:
    """顺序执行一个 host 测试的命令并检查退出状态和输出。"""

    chunks: list[str] = []
    for command in test.commands:
        completed = subprocess.run(
            command,
            cwd=REPO_ROOT,
            shell=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=test.timeout,
            check=False,
        )
        chunks.append(f"$ {command}\n{completed.stdout}")
        if completed.returncode != 0:
            output = "\n".join(chunks)
            log_path = _write_log(suite, test.name, output)
            raise TestFailure(
                f"host command exited with {completed.returncode}: {command}; log: {log_path}"
            )
    output = "\n".join(chunks)
    log_path = _write_log(suite, test.name, output)
    _assert_output(test, output)
    print(f"PASS {test.name} ({log_path.relative_to(REPO_ROOT)})")


def _start_qemu(cpus: int) -> pexpect.spawn:
    """启动使用 snapshot 的 xv6 QEMU，并等待 shell 提示符。"""

    command = (
        "make -s --no-print-directory qemu "
        f"CPUS={cpus} QEMUEXTRA=-snapshot"
    )
    child = pexpect.spawn(
        "/bin/bash",
        ["-lc", command],
        cwd=str(REPO_ROOT),
        encoding="utf-8",
        codec_errors="replace",
        timeout=120,
    )
    try:
        child.expect_exact(QEMU_SHELL_PROMPT)
    except (pexpect.TIMEOUT, pexpect.EOF):
        child.terminate(force=True)
        raise
    return child


def _stop_qemu(child: pexpect.spawn) -> None:
    """通过 QEMU monitor 快捷键终止实例，超时时强制清理。"""

    if not child.isalive():
        return
    child.sendcontrol("a")
    child.send("x")
    try:
        child.expect(pexpect.EOF, timeout=5)
    except (pexpect.TIMEOUT, pexpect.EOF):
        child.terminate(force=True)


def _wait_for_qemu_command(child: pexpect.spawn) -> QemuCommandResult:
    """等待 guest 命令返回 shell，并对已知内核致命输出快速失败。

    Args:
        child: 已启动并进入 xv6 shell 的 pexpect 子进程；函数会消费本次命令输出。

    Returns:
        返回已捕获输出和可选失败原因。出现 prompt 时 failure 为 None；出现
        panic、kerneltrap、EOF 或 timeout 时返回可直接写入 TestFailure 的原因。
    """

    expectations = (
        QEMU_SHELL_PROMPT,
        *QEMU_FATAL_OUTPUTS,
        pexpect.EOF,
        pexpect.TIMEOUT,
    )
    matched = child.expect_exact(expectations)
    output = child.before or ""

    if matched == 0:
        return QemuCommandResult(output=output)

    fatal_end = 1 + len(QEMU_FATAL_OUTPUTS)
    if matched < fatal_end:
        fatal = QEMU_FATAL_OUTPUTS[matched - 1]
        output += child.after or fatal

        # panic 文本本身已经足以判定失败；额外最多等待一秒读取该行余下内容，
        # 使日志保留 `panic: acquire` 这类真正用于定位根因的信息。
        tail_match = child.expect_exact(("\n", pexpect.EOF, pexpect.TIMEOUT), timeout=1)
        output += child.before or ""
        if tail_match == 0:
            output += child.after or "\n"
        return QemuCommandResult(
            output=output,
            failure=f"matched fatal output: {fatal}",
        )

    if matched == fatal_end:
        return QemuCommandResult(
            output=output,
            failure="QEMU exited before returning to the shell",
        )

    return QemuCommandResult(
        output=output,
        failure="QEMU did not return to the shell before timeout",
    )


def _run_qemu_tests(suite: str, tests: Sequence[TestCase], cpus: int) -> None:
    """在一个原子 suite 的 QEMU snapshot 内顺序执行其 guest 命令。"""

    child = _start_qemu(cpus)
    boot_output = child.before
    try:
        for test in tests:
            chunks = [boot_output]
            for command in test.commands:
                child.timeout = test.timeout
                child.sendline(command)
                result = _wait_for_qemu_command(child)
                chunks.append(f"$ {command}\n{result.output}")
                if result.failure is not None:
                    output = "\n".join(chunks)
                    log_path = _write_log(suite, test.name, output)
                    raise TestFailure(f"{result.failure}; log: {log_path}")
            output = "\n".join(chunks)
            log_path = _write_log(suite, test.name, output)
            _assert_output(test, output)
            print(f"PASS {test.name} ({log_path.relative_to(REPO_ROOT)})")
    finally:
        _stop_qemu(child)


def _expand_suite(name: str, stack: tuple[str, ...] = ()) -> list[str]:
    """递归展开聚合 suite，并检测未知引用和循环依赖。"""

    if name not in SUITES:
        raise KeyError(name)
    if name in stack:
        raise RuntimeError(f"cyclic suite include: {' -> '.join(stack + (name,))}")
    suite = SUITES[name]
    if not suite.includes:
        return [name]
    expanded: list[str] = []
    for child in suite.includes:
        expanded.extend(_expand_suite(child, stack + (name,)))
    return expanded


def _deduplicate(values: Iterable[str]) -> list[str]:
    """按首次出现顺序去重 suite 名称。"""

    result: list[str] = []
    seen: set[str] = set()
    for value in values:
        if value not in seen:
            seen.add(value)
            result.append(value)
    return result


def run_atomic_suite(name: str, cpus: int) -> None:
    """执行一个原子 suite，host 测试先于 QEMU 测试运行。"""

    suite = SUITES[name]
    print(f"\n== Suite {name}: {suite.description} ==")
    host_tests = [test for test in suite.tests if test.host]
    qemu_tests = [test for test in suite.tests if not test.host]
    for test in host_tests:
        _run_host_test(name, test)
    if qemu_tests:
        _run_qemu_tests(name, qemu_tests, cpus)


def parse_args() -> argparse.Namespace:
    """解析 suite、CPU 数量和列表模式参数。"""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--suite",
        action="append",
        dest="suites",
        help="suite name; may be supplied more than once",
    )
    parser.add_argument("--cpus", type=int, default=3, help="QEMU CPU count")
    parser.add_argument("--list", action="store_true", help="list available suites")
    return parser.parse_args()


def main() -> int:
    """执行请求的 suite 并以进程退出状态汇总全部失败。"""

    args = parse_args()
    if args.list:
        for name in sorted(SUITES):
            suite = SUITES[name]
            kind = "aggregate" if suite.includes else "atomic"
            print(f"{name:20} {kind:9} {suite.description}")
        return 0
    if not args.suites:
        raise SystemExit("at least one --suite is required")
    if args.cpus < 1:
        raise SystemExit("--cpus must be at least 1")

    requested: list[str] = []
    for name in args.suites:
        try:
            requested.extend(_expand_suite(name))
        except KeyError as exc:
            raise SystemExit(f"unknown suite: {exc.args[0]}") from exc

    failures: list[str] = []
    for name in _deduplicate(requested):
        try:
            run_atomic_suite(name, args.cpus)
        except (TestFailure, subprocess.TimeoutExpired) as exc:
            failures.append(f"{name}: {exc}")
            print(f"FAIL {name}: {exc}", file=sys.stderr)

    if failures:
        print("\nFailed suites:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1

    print("\nAll requested suites passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
