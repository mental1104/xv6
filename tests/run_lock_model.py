#!/usr/bin/env python3
"""在单核和请求的多核配置下执行锁模型 guest 回归。"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence

from run import GUEST_SUCCESS, TestCase, TestFailure, _run_qemu_tests


def cpu_matrix(requested_cpus: int) -> tuple[int, ...]:
    """返回去重后的验证矩阵，始终先覆盖 CPUS=1。"""

    if requested_cpus < 1:
        raise ValueError("CPU count must be at least 1")
    return tuple(dict.fromkeys((1, requested_cpus)))


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
    """执行单核与多核矩阵，并以进程状态汇总失败。"""

    args = parse_args(argv)
    try:
        matrix = cpu_matrix(args.cpus)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc

    failures: list[str] = []
    for cpus in matrix:
        try:
            run_lock_model(cpus)
        except TestFailure as exc:
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
