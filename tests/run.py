#!/usr/bin/env python3
"""通过 QEMU 或宿主机执行稳定的 xv6 回归测试套件。"""

from __future__ import annotations

import argparse
import re
import shlex
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

import pexpect


REPO_ROOT = Path(__file__).resolve().parents[1]
RESULT_ROOT = REPO_ROOT / "test-results"
ANSI_ORANGE_BOLD = "\x1b[1;38;5;208m"
ANSI_BLUE_BOLD = "\x1b[1;34m"
ANSI_RESET = "\x1b[0m"
# PID 1 在首个 Shell 前切换到 /root；精确匹配完整 ANSI 提示符，避免命令输出中的
# 普通 `# ` 或颜色复位序列被误判为下一次输入边界。
QEMU_SHELL_PROMPT = (
    f"{ANSI_ORANGE_BOLD}root@xv6{ANSI_RESET}:"
    f"{ANSI_BLUE_BOLD}/root{ANSI_RESET}# "
)
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
GUEST_PROGRAM_PATHS = {
    "xv6test": "/usr/bin/xv6test",
}
LOG_RECOVERY_MARKER = "LOGRECOVER replay entries="


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
class CrashRecoveryScenario:
    """描述一个需要真实磁盘镜像跨 QEMU 重启保存的日志崩溃场景。"""

    name: str
    prepare_test: str
    verify_test: str
    crash_marker: str
    recovery_expected: bool
    timeout: int = 120


@dataclass(frozen=True)
class Suite:
    """描述一个原子测试套件或由其他套件组成的聚合套件。"""

    name: str
    tests: tuple[TestCase, ...] = ()
    includes: tuple[str, ...] = ()
    description: str = ""
    crash_scenarios: tuple[CrashRecoveryScenario, ...] = ()


@dataclass(frozen=True)
class QemuCommandResult:
    """记录一次 guest 命令等待 Shell prompt 的输出和失败原因。"""

    output: str
    failure: str | None = None


LOG_RECOVERY_SCENARIOS = (
    CrashRecoveryScenario(
        name="before-commit",
        prepare_test="logcrash-before-prepare",
        verify_test="logcrash-before-verify",
        crash_marker="LOGCRASH injected phase=before-commit",
        recovery_expected=False,
    ),
    CrashRecoveryScenario(
        name="after-commit",
        prepare_test="logcrash-after-prepare",
        verify_test="logcrash-after-verify",
        crash_marker="LOGCRASH injected phase=after-commit",
        recovery_expected=True,
    ),
    CrashRecoveryScenario(
        name="during-install",
        prepare_test="logcrash-install-prepare",
        verify_test="logcrash-install-verify",
        crash_marker="LOGCRASH injected phase=during-install",
        recovery_expected=True,
    ),
)


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
            # 两个快速项仍在同一个 QEMU snapshot 中顺序执行。拆开看门狗和日志
            # 只为精确定位慢路径或阻塞点，不通过重启 guest 隐藏跨测试状态问题。
            TestCase(
                "lab8-createdelete",
                ("xv6test --run lab8-createdelete",),
                expected=GUEST_SUCCESS,
                timeout=180,
            ),
            TestCase(
                "lab8-fourfiles",
                ("xv6test --run lab8-fourfiles",),
                expected=GUEST_SUCCESS,
                timeout=180,
            ),
        ),
    ),
    "lab9-bigfile": Suite(
        name="lab9-bigfile",
        description="Lab9 large-file and persistent journal recovery regression",
        tests=(
            TestCase(
                "lab9-bigfile",
                ("xv6test --run lab9-bigfile",),
                expected=GUEST_SUCCESS,
                timeout=120,
            ),
            TestCase(
                "logcrash-api",
                ("xv6test --run logcrash-api",),
                expected=GUEST_SUCCESS,
                timeout=120,
            ),
        ),
        crash_scenarios=LOG_RECOVERY_SCENARIOS,
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


def _absolute_guest_command(command: str) -> str:
    """把 guest 命令首程序名替换为固定绝对路径。

    Args:
        command: suite 中保存的可读命令文本。首 token 可以是已登记程序名，后续参数
            与重定向文本保持原样。

    Returns:
        实际发送给 xv6 Shell 的命令。未知程序原样返回，不做 PATH 或目录遍历搜索。
    """

    program, separator, remainder = command.partition(" ")
    absolute = GUEST_PROGRAM_PATHS.get(program, program)
    return f"{absolute}{separator}{remainder}"


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

    started = time.perf_counter()
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
                f"host command exited with {completed.returncode}: {command}; "
                f"log: {log_path}"
            )
    output = "\n".join(chunks)
    log_path = _write_log(suite, test.name, output)
    _assert_output(test, output)
    elapsed = time.perf_counter() - started
    print(f"PASS {test.name} {elapsed:.2f}s ({log_path.relative_to(REPO_ROOT)})")


def _build_qemu_command(
    cpus: int,
    *,
    fsimg: Path | None = None,
    snapshot: bool = True,
) -> str:
    """构造显式 CPU、磁盘和 snapshot 策略的 QEMU make 命令。

    Args:
        cpus: QEMU 虚拟 CPU 数量。
        fsimg: 需要跨重启保留写入的镜像；为空时使用 Makefile 默认 fs.img。
        snapshot: 为 True 时把磁盘写入限制在当前 QEMU 进程。

    Returns:
        可由 ``bash -lc`` 执行的命令文本。
    """

    parts = ["make", "-s", "--no-print-directory", "qemu", f"CPUS={cpus}"]
    if fsimg is not None:
        parts.append(f"FSIMG={shlex.quote(str(fsimg))}")
    if snapshot:
        parts.append("QEMUEXTRA=-snapshot")
    return " ".join(parts)


def _start_qemu(
    cpus: int,
    *,
    fsimg: Path | None = None,
    snapshot: bool = True,
) -> pexpect.spawn:
    """启动指定磁盘策略的 xv6 QEMU，并等待 Shell 提示符。"""

    command = _build_qemu_command(cpus, fsimg=fsimg, snapshot=snapshot)
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


def _capture_qemu_procdump(child: pexpect.spawn) -> str:
    """在 guest 命令超时时触发 Ctrl-P，并有界收集进程状态。

    Args:
        child: 仍在运行的 QEMU pexpect 子进程。函数会向 guest console 发送
            Ctrl-P，并消费随后到达的诊断输出。

    Returns:
        带稳定标题的诊断文本。若 guest 未产生输出，也会返回明确占位信息，
        使日志能够区分“未采集”与“采集后为空”。

    Ctrl-P 是 xv6 保留的 procdump 控制键。这里最多执行 20 次 0.1 秒读取，
    避免诊断本身无限阻塞原看门狗的失败收敛路径。
    """

    child.sendcontrol("p")
    chunks: list[str] = []
    for _ in range(20):
        try:
            chunk = child.read_nonblocking(size=4096, timeout=0.1)
        except (pexpect.TIMEOUT, pexpect.EOF):
            break
        if chunk:
            chunks.append(chunk)

    diagnostic = "".join(chunks)
    if not diagnostic:
        diagnostic = "<Ctrl-P produced no process output>\n"
    return "\nXV6TEST timeout diagnostics (Ctrl-P procdump):\n" + diagnostic


def _wait_for_qemu_command(child: pexpect.spawn) -> QemuCommandResult:
    """等待 guest 命令返回 Shell，并对已知内核致命输出快速失败。

    Args:
        child: 已启动并进入 xv6 Shell 的 pexpect 子进程；函数会消费本次命令输出。

    Returns:
        返回已捕获输出和可选失败原因。出现 prompt 时 failure 为 None；出现
        panic、kerneltrap、EOF 或 timeout 时返回可直接写入 TestFailure 的原因。
        timeout 还会附带一次有界 Ctrl-P procdump，供定位阻塞进程和 sleep channel。
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

    output += _capture_qemu_procdump(child)
    return QemuCommandResult(
        output=output,
        failure="QEMU did not return to the shell before timeout",
    )


def _run_qemu_tests(suite: str, tests: Sequence[TestCase], cpus: int) -> None:
    """在一个原子 suite 的同一 QEMU snapshot 内顺序执行全部 guest 命令。"""

    child = _start_qemu(cpus)
    boot_output = child.before
    try:
        for test in tests:
            started = time.perf_counter()
            chunks = [boot_output]
            for command in test.commands:
                guest_command = _absolute_guest_command(command)
                child.timeout = test.timeout
                child.sendline(guest_command)
                result = _wait_for_qemu_command(child)
                chunks.append(f"$ {guest_command}\n{result.output}")
                if result.failure is not None:
                    output = "\n".join(chunks)
                    log_path = _write_log(suite, test.name, output)
                    raise TestFailure(f"{result.failure}; log: {log_path}")
            output = "\n".join(chunks)
            log_path = _write_log(suite, test.name, output)
            _assert_output(test, output)
            elapsed = time.perf_counter() - started
            print(f"PASS {test.name} {elapsed:.2f}s ({log_path.relative_to(REPO_ROOT)})")
    finally:
        _stop_qemu(child)


def _crash_cpu_variants(cpus: int) -> tuple[int, ...]:
    """多核为主证据，同时补充单核恢复；请求本身为单核时避免重复。"""

    return (cpus,) if cpus == 1 else (cpus, 1)


def _run_expected_log_crash(
    suite: str,
    scenario: CrashRecoveryScenario,
    cpus: int,
    fsimg: Path,
) -> None:
    """触发指定提交阶段，并断言 QEMU 只因预期日志注入 panic。"""

    child = _start_qemu(cpus, fsimg=fsimg, snapshot=False)
    boot_output = child.before
    command = _absolute_guest_command(f"xv6test --run {scenario.prepare_test}")
    try:
        child.timeout = scenario.timeout
        child.sendline(command)
        result = _wait_for_qemu_command(child)
        output = f"{boot_output}\n$ {command}\n{result.output}"
        log_path = _write_log(
            suite,
            f"{scenario.name}-cpu{cpus}-crash",
            output,
        )
        if result.failure != "matched fatal output: panic:":
            raise TestFailure(
                f"{scenario.name} did not stop at the expected panic; log: {log_path}"
            )
        if scenario.crash_marker not in _normalize_output(output):
            raise TestFailure(
                f"{scenario.name} missing crash marker {scenario.crash_marker!r}; "
                f"log: {log_path}"
            )
    finally:
        _stop_qemu(child)


def _run_log_recovery_verification(
    suite: str,
    scenario: CrashRecoveryScenario,
    cpus: int,
    fsimg: Path,
    *,
    first_recovery: bool,
) -> None:
    """重启同一镜像，检查恢复触发条件并执行 guest 原子性 oracle。"""

    child = _start_qemu(cpus, fsimg=fsimg, snapshot=False)
    boot_output = child.before
    command = _absolute_guest_command(f"xv6test --run {scenario.verify_test}")
    stage = "recovery" if first_recovery else "idempotent-reboot"
    try:
        normalized_boot = _normalize_output(boot_output)
        should_replay = first_recovery and scenario.recovery_expected
        if should_replay and LOG_RECOVERY_MARKER not in normalized_boot:
            log_path = _write_log(
                suite,
                f"{scenario.name}-cpu{cpus}-{stage}",
                boot_output,
            )
            raise TestFailure(
                f"{scenario.name} expected recovery replay but marker was absent; "
                f"log: {log_path}"
            )
        if not should_replay and LOG_RECOVERY_MARKER in normalized_boot:
            log_path = _write_log(
                suite,
                f"{scenario.name}-cpu{cpus}-{stage}",
                boot_output,
            )
            raise TestFailure(
                f"{scenario.name} replayed an uncommitted or already-cleared log; "
                f"log: {log_path}"
            )

        child.timeout = scenario.timeout
        child.sendline(command)
        result = _wait_for_qemu_command(child)
        output = f"{boot_output}\n$ {command}\n{result.output}"
        log_path = _write_log(
            suite,
            f"{scenario.name}-cpu{cpus}-{stage}",
            output,
        )
        if result.failure is not None:
            raise TestFailure(f"{result.failure}; log: {log_path}")
        _assert_output(
            TestCase(
                name=scenario.verify_test,
                commands=(command,),
                expected=GUEST_SUCCESS,
            ),
            output,
        )
    finally:
        _stop_qemu(child)


def _run_crash_recovery_scenario(
    suite: str,
    scenario: CrashRecoveryScenario,
    cpus: int,
) -> None:
    """在独立镜像上完成崩溃、首次恢复和第二次幂等重启闭环。"""

    started = time.perf_counter()
    directory = RESULT_ROOT / _safe_name(suite)
    directory.mkdir(parents=True, exist_ok=True)
    fsimg = directory / f"{_safe_name(scenario.name)}-cpu{cpus}.img"
    shutil.copyfile(REPO_ROOT / "fs.img", fsimg)
    try:
        _run_expected_log_crash(suite, scenario, cpus, fsimg)
        _run_log_recovery_verification(
            suite,
            scenario,
            cpus,
            fsimg,
            first_recovery=True,
        )
        _run_log_recovery_verification(
            suite,
            scenario,
            cpus,
            fsimg,
            first_recovery=False,
        )
    finally:
        fsimg.unlink(missing_ok=True)

    elapsed = time.perf_counter() - started
    print(f"PASS log-recovery-{scenario.name}-cpu{cpus} {elapsed:.2f}s")


def _run_crash_recovery_scenarios(
    suite: str,
    scenarios: Sequence[CrashRecoveryScenario],
    cpus: int,
) -> None:
    """按多核主证据和单核补充证据执行全部持久日志场景。"""

    for cpu_count in _crash_cpu_variants(cpus):
        for scenario in scenarios:
            _run_crash_recovery_scenario(suite, scenario, cpu_count)


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
    """执行原子 suite：host、snapshot guest、持久崩溃矩阵依次运行。"""

    suite = SUITES[name]
    print(f"\n== Suite {name}: {suite.description} ==")
    host_tests = [test for test in suite.tests if test.host]
    qemu_tests = [test for test in suite.tests if not test.host]
    for test in host_tests:
        _run_host_test(name, test)
    if qemu_tests:
        _run_qemu_tests(name, qemu_tests, cpus)
    if suite.crash_scenarios:
        _run_crash_recovery_scenarios(name, suite.crash_scenarios, cpus)


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
