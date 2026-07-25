#!/usr/bin/env python3
"""采集“教材分段概念 → xv6 逻辑区域与 Sv39 分页”的可归档证据。"""

from __future__ import annotations

import argparse
from pathlib import Path

import run as runner


SUITE_NAME = "segmentation-mapping"
TEST_NAME = "segmentation-mapping"

# 业务断言由 guest 内的 memviztest 持有；host 仅编排 QEMU、保留原始输出并检查
# XV6TEST 的稳定结束协议，避免把内存模型语义重新搬到 Python。
EXPERIMENT = runner.TestCase(
    name=TEST_NAME,
    commands=(
        "/usr/bin/memviz user --plain",
        "/usr/bin/memviz pagetable user --plain",
        "/usr/bin/memviz pagetable guard --plain",
        "/usr/bin/memviz pagetable stack --plain",
        "/usr/bin/memviztest regions",
        "/usr/bin/xv6test --run lab3-memviz",
    ),
    expected=runner.GUEST_SUCCESS,
    timeout=700,
)


def parse_args() -> argparse.Namespace:
    """解析 QEMU CPU 数量。"""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, default=3, help="QEMU CPU count")
    return parser.parse_args()


def evidence_log_path() -> Path:
    """返回本实验使用的稳定原始日志路径。"""

    return runner.RESULT_ROOT / SUITE_NAME / f"{TEST_NAME}.log"


def main() -> int:
    """运行单个 QEMU snapshot，并报告可直接归档的原始日志。"""

    args = parse_args()
    if args.cpus < 1:
        raise SystemExit("--cpus must be at least 1")

    runner._run_qemu_tests(SUITE_NAME, (EXPERIMENT,), args.cpus)
    log_path = evidence_log_path().relative_to(runner.REPO_ROOT)
    print(f"Segmentation mapping evidence: {log_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
