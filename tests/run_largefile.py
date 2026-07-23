#!/usr/bin/env python3
"""为稀疏大镜像注册并执行显式 4 GiB xv6 回归。"""

from __future__ import annotations

import sys
from pathlib import Path


TEST_ROOT = Path(__file__).resolve().parent
if str(TEST_ROOT) not in sys.path:
    sys.path.insert(0, str(TEST_ROOT))

import run as runner


runner.SUITES["largefs-4gib"] = runner.Suite(
    name="largefs-4gib",
    description="Explicit 4-GiB sequential write/read/stat/unlink/reuse regression",
    tests=(
        runner.TestCase(
            "largefs-4gib",
            ("xv6test --run largefs-4gib",),
            expected=runner.GUEST_SUCCESS,
            timeout=43_200,
        ),
    ),
)


if __name__ == "__main__":
    raise SystemExit(runner.main())
