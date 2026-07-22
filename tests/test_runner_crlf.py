#!/usr/bin/env python3
"""验证 QEMU CRLF 输出不会破坏 guest-first 协议匹配。"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


RUNNER_PATH = Path(__file__).with_name("run.py")
SPEC = importlib.util.spec_from_file_location("xv6_regression_runner_crlf", RUNNER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load regression runner from {RUNNER_PATH}")
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


class QemuLineEndingTests(unittest.TestCase):
    """覆盖 pexpect 从 QEMU 收到的 CRLF 与裸 CR。"""

    def test_guest_success_protocol_accepts_crlf(self) -> None:
        test = RUNNER.TestCase(
            name="guest",
            commands=("xv6test --group lab3",),
            expected=RUNNER.GUEST_SUCCESS,
        )

        RUNNER._assert_output(
            test,
            "XV6TEST summary selected=4 passed=4 failed=0\r\n"
            "XV6TEST done status=0\r\n",
        )

    def test_counted_and_rejected_patterns_use_normalized_lines(self) -> None:
        counted = RUNNER.TestCase(
            name="frames",
            commands=("xv6test --group lab4",),
            expected=RUNNER.GUEST_SUCCESS,
            counted=(RUNNER.CountExpectation(r"^0x[0-9a-f]+$", 2),),
        )
        RUNNER._assert_output(
            counted,
            "0x80001234\r0x80005678\rXV6TEST done status=0\r",
        )

        rejected = RUNNER.TestCase(name="panic", commands=("x",))
        with self.assertRaisesRegex(RUNNER.TestFailure, "matched rejected pattern"):
            RUNNER._assert_output(rejected, "panic: broken invariant\r\n")


if __name__ == "__main__":
    unittest.main()
