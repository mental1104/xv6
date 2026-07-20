#!/usr/bin/env python3
"""Run stable xv6 regression suites through QEMU or on the host."""

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
DEFAULT_REJECTED = (
    r"panic:",
    r"kerneltrap",
    r"exec .* failed",
    r"\bFAIL(?:ED)?\b",
)


@dataclass(frozen=True)
class CountExpectation:
    pattern: str
    minimum: int


@dataclass(frozen=True)
class TestCase:
    name: str
    commands: tuple[str, ...]
    expected: tuple[str, ...] = ()
    counted: tuple[CountExpectation, ...] = ()
    rejected: tuple[str, ...] = DEFAULT_REJECTED
    timeout: int = 90
    host: bool = False


@dataclass(frozen=True)
class Suite:
    name: str
    tests: tuple[TestCase, ...] = ()
    includes: tuple[str, ...] = ()
    description: str = ""


SUITES: dict[str, Suite] = {
    "lab-basic": Suite(
        name="lab-basic",
        description="Lab1 util and Lab2 syscall behavior",
        tests=(
            TestCase("lab1-sleep", ("sleep 1",)),
            TestCase(
                "lab1-pingpong",
                ("pingpong",),
                expected=(r"\d+: received ping", r"\d+: received pong"),
            ),
            TestCase(
                "lab1-primes",
                ("primes",),
                expected=(r"^prime 2$", r"^prime 31$"),
            ),
            TestCase(
                "lab1-find",
                ("mkdir ci_find", "echo x > ci_find/needle", "find . needle"),
                expected=(r"^\./ci_find/needle$",),
            ),
            TestCase(
                "lab1-xargs",
                ("echo hello too | xargs echo bye",),
                expected=(r"^bye hello too$",),
            ),
            TestCase(
                "lab2-trace",
                ("trace 32 grep hello README",),
                expected=(r"syscall read ->",),
            ),
            TestCase(
                "lab2-sysinfo",
                ("sysinfotest",),
                expected=(r"^sysinfotest: OK$",),
            ),
        ),
    ),
    "lab-vm": Suite(
        name="lab-vm",
        description="Lab3 page-table paths, Lab4 traps, Lab5 lazy allocation and Lab6 COW",
        tests=(
            TestCase(
                "lab3-copyin",
                ("usertests copyin",),
                expected=(r"^ALL TESTS PASSED$",),
                timeout=180,
            ),
            TestCase(
                "lab3-copyout",
                ("usertests copyout",),
                expected=(r"^ALL TESTS PASSED$",),
                timeout=180,
            ),
            TestCase(
                "lab3-copyinstr",
                ("usertests copyinstr1",),
                expected=(r"^ALL TESTS PASSED$",),
                timeout=180,
            ),
            TestCase(
                "lab3-address-space-growth",
                ("usertests sbrkmuch",),
                expected=(r"^ALL TESTS PASSED$",),
                timeout=240,
            ),
            TestCase(
                "lab4-backtrace",
                ("bttest",),
                counted=(CountExpectation(r"^0x[0-9a-f]+$", 3),),
            ),
            TestCase(
                "lab4-alarm",
                ("alarmtest",),
                expected=(r"^test0 passed$", r"^\.?test1 passed$", r"^\.?test2 passed$"),
                timeout=180,
            ),
            TestCase(
                "lab5-lazy-allocation",
                ("lazytests",),
                expected=(r"^test lazy unmap: OK$", r"^test lazy alloc: OK$"),
                timeout=240,
            ),
            TestCase(
                "lab6-copy-on-write",
                ("cowtest",),
                expected=(r"^ALL COW TESTS PASSED$",),
                timeout=300,
            ),
        ),
    ),
    "lab7-thread": Suite(
        name="lab7-thread",
        description="Lab7 user threads and host pthread exercises",
        tests=(
            TestCase(
                "lab7-uthread",
                ("uthread",),
                expected=(r"^thread_schedule: no runnable threads$",),
                timeout=180,
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
        description="Lab8 allocator and buffer-cache behavioral regression without unstable contention thresholds",
        tests=(
            TestCase(
                "lab8-kalloc-behavior",
                ("usertests sbrkmuch",),
                expected=(r"^ALL TESTS PASSED$",),
                timeout=240,
            ),
            TestCase(
                "lab8-bcache-create-delete",
                ("usertests createdelete",),
                expected=(r"^ALL TESTS PASSED$",),
                timeout=240,
            ),
            TestCase(
                "lab8-bcache-concurrent-files",
                ("usertests fourfiles",),
                expected=(r"^ALL TESTS PASSED$",),
                timeout=240,
            ),
            TestCase(
                "lab8-bcache-big-write",
                ("usertests bigwrite",),
                expected=(r"^ALL TESTS PASSED$",),
                timeout=240,
            ),
        ),
    ),
    "lab9-bigfile": Suite(
        name="lab9-bigfile",
        description="Lab9 large-file mapping on an isolated disk snapshot",
        tests=(
            TestCase(
                "lab9-bigfile",
                ("bigfile",),
                expected=(r"^wrote 65803 blocks$", r"^bigfile done; ok$"),
                timeout=360,
            ),
        ),
    ),
    "lab9-symlink": Suite(
        name="lab9-symlink",
        description="Lab9 symbolic-link behavior on an isolated disk snapshot",
        tests=(
            TestCase(
                "lab9-symlink",
                ("symlinktest",),
                expected=(r"^test symlinks: ok$", r"^test concurrent symlinks: ok$"),
                timeout=180,
            ),
        ),
    ),
    "lab10-mmap": Suite(
        name="lab10-mmap",
        description="Lab10 mmap behavior on an isolated disk snapshot",
        tests=(
            TestCase(
                "lab10-mmap",
                ("mmaptest",),
                expected=(
                    r"^test mmap f: OK$",
                    r"^test mmap private: OK$",
                    r"^test mmap read/write: OK$",
                    r"^test mmap dirty: OK$",
                    r"^fork_test OK$",
                ),
                timeout=360,
            ),
        ),
    ),
    "usertests-core": Suite(
        name="usertests-core",
        description="Focused cross-lab regression used on pull requests",
        tests=tuple(
            TestCase(
                f"usertests-{name}",
                (f"usertests {name}",),
                expected=(r"^ALL TESTS PASSED$",),
                timeout=240,
            )
            for name in (
                "sbrkbugs",
                "forkforkfork",
                "copyin",
                "copyout",
                "copyinstr1",
                "createdelete",
                "linkunlink",
                "openiput",
            )
        ),
    ),
    "usertests-full": Suite(
        name="usertests-full",
        description="Complete xv6 usertests regression",
        tests=(
            TestCase(
                "usertests-full",
                ("usertests",),
                expected=(r"^ALL TESTS PASSED$",),
                timeout=900,
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
    pass


def _safe_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "-", value)


def _assert_output(test: TestCase, output: str) -> None:
    flags = re.MULTILINE
    for pattern in test.rejected:
        if re.search(pattern, output, flags):
            raise TestFailure(f"matched rejected pattern: {pattern}")
    for pattern in test.expected:
        if not re.search(pattern, output, flags):
            raise TestFailure(f"missing expected pattern: {pattern}")
    for expectation in test.counted:
        count = len(re.findall(expectation.pattern, output, flags))
        if count < expectation.minimum:
            raise TestFailure(
                f"pattern {expectation.pattern!r} matched {count} times; "
                f"expected at least {expectation.minimum}"
            )


def _write_log(suite: str, test: str, output: str) -> Path:
    directory = RESULT_ROOT / _safe_name(suite)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{_safe_name(test)}.log"
    path.write_text(output, encoding="utf-8", errors="replace")
    return path


def _run_host_test(suite: str, test: TestCase) -> None:
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
    child.expect_exact("$ ")
    return child


def _stop_qemu(child: pexpect.spawn) -> None:
    if not child.isalive():
        return
    child.sendcontrol("a")
    child.send("x")
    try:
        child.expect(pexpect.EOF, timeout=5)
    except (pexpect.TIMEOUT, pexpect.EOF):
        child.terminate(force=True)


def _run_qemu_tests(suite: str, tests: Sequence[TestCase], cpus: int) -> None:
    child = _start_qemu(cpus)
    boot_output = child.before
    try:
        for test in tests:
            chunks = [boot_output]
            try:
                for command in test.commands:
                    child.timeout = test.timeout
                    child.sendline(command)
                    child.expect_exact("$ ")
                    chunks.append(f"$ {command}\n{child.before}")
                output = "\n".join(chunks)
                log_path = _write_log(suite, test.name, output)
                _assert_output(test, output)
                print(f"PASS {test.name} ({log_path.relative_to(REPO_ROOT)})")
            except (pexpect.TIMEOUT, pexpect.EOF) as exc:
                chunks.append(child.before or "")
                output = "\n".join(chunks)
                log_path = _write_log(suite, test.name, output)
                raise TestFailure(f"QEMU did not return to the shell; log: {log_path}") from exc
    finally:
        _stop_qemu(child)


def _expand_suite(name: str, stack: tuple[str, ...] = ()) -> list[str]:
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
    result: list[str] = []
    seen: set[str] = set()
    for value in values:
        if value not in seen:
            seen.add(value)
            result.append(value)
    return result


def run_atomic_suite(name: str, cpus: int) -> None:
    suite = SUITES[name]
    print(f"\n== Suite {name}: {suite.description} ==")
    host_tests = [test for test in suite.tests if test.host]
    qemu_tests = [test for test in suite.tests if not test.host]
    for test in host_tests:
        _run_host_test(name, test)
    if qemu_tests:
        _run_qemu_tests(name, qemu_tests, cpus)


def parse_args() -> argparse.Namespace:
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
