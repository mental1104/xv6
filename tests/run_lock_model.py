#!/usr/bin/env python3
"""在单核和请求的多核配置下重建并执行锁模型 guest 回归。"""

from __future__ import annotations

import argparse
import subprocess
import sys
from collections.abc import Sequence

import pexpect

from run import GUEST_SUCCESS, REPO_ROOT, TestCase, TestFailure, _run_qemu_tests


def cpu_matrix(requested_cpus: int) -> tuple[int, ...]:
    """返回去重后的验证矩阵，始终先覆盖 CPUS=1。"""

    if requested_cpus < 1:
        raise ValueError("CPU count must be at least 1")
    return tuple(dict.fromkeys((1, requested_cpus)))


def build_xv6(cpus: int) -> None:
    """按指定 CPU 数量执行干净构建，避免复用其他矩阵项的内核。"""

    subprocess.run(("make", "clean"), cwd=REPO_ROOT, check=True)
    subprocess.run(("make", "-j2", f"CPUS={cpus}"), cwd=REPO_ROOT, check=True)


def run_lock_model(cpus: int) -> None:
    """在一个全新 QEMU snapshot 中运行锁模型测试。"""

    test = TestCase(
        name=f"lab8-lock-model-cpu{cpus}",
        commands=("xv6test --run lab8-lock-model",),
        expected=GUEST_SUCCESS,
        timeout=180,
    )
    _run_qemu_tests(f"lock-model-cpu{cpus}", (test,), cpus)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    """解析调用者希望验证的默认多核 CPU 数量。"""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, default=3, help="default multi-core QEMU CPU count")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    """逐项干净构建并执行单核与多核矩阵。"""

    args = parse_args(argv)
    try:
        matrix = cpu_matrix(args.cpus)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc

    failures: list[str] = []
    for cpus in matrix:
        try:
            build_xv6(cpus)
            run_lock_model(cpus)
        except (TestFailure, subprocess.CalledProcessError, pexpect.TIMEOUT, pexpect.EOF) as exc:
            failures.append(f"CPUS={cpus}: {exc}")
            print(f"FAIL lock model CPUS={cpus}: {exc}", file=sys.stderr)

    if failures:
        print("\nLock model failures:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1

    print("\nLock model CPU matrix passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
