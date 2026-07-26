#!/usr/bin/env python3
"""以 opt-in makefile 启动 RAID1 实验，不改变默认 xv6 构建入口。"""

from __future__ import annotations

import shlex
from pathlib import Path

import raid1_experiment as experiment


def _qemu_command(
    root_image: Path,
    member0: Path | None,
    member1: Path | None,
    cpus: int,
) -> str:
    """构造显式加载 tests/raid1.mk 的 QEMU 命令。"""

    parts = [
        "make",
        "-s",
        "--no-print-directory",
        "-f",
        "Makefile",
        "-f",
        "tests/raid1.mk",
        "qemu",
        f"CPUS={cpus}",
        f"FSIMG={root_image}",
    ]
    if member0 is not None:
        parts.append(f"RAID1_MEMBER0={member0}")
    if member1 is not None:
        parts.append(f"RAID1_MEMBER1={member1}")
    return " ".join(shlex.quote(part) for part in parts)


def main() -> int:
    """切换实验入口后委托原始状态机执行。"""

    experiment.TEST_PROGRAM = "/raid1test"
    experiment._qemu_command = _qemu_command
    return experiment.main()


if __name__ == "__main__":
    raise SystemExit(main())
