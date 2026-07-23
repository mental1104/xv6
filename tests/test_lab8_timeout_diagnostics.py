#!/usr/bin/env python3
"""验证 Lab8 回归拆分和 QEMU 超时诊断协议。"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from unittest import mock


RUNNER_PATH = Path(__file__).with_name("run.py")
SPEC = importlib.util.spec_from_file_location("xv6_lab8_runner", RUNNER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load regression runner from {RUNNER_PATH}")
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


class Lab8SuiteTests(unittest.TestCase):
    """验证 Lab8 用例保持同一 suite，同时拥有独立看门狗和日志名称。"""

    def test_lab8_cases_are_individually_addressable(self) -> None:
        """确保 PR 快速项不再被一个 group 级 600 秒看门狗包裹。"""

        tests = RUNNER.SUITES["lab8-locks"].tests
        self.assertEqual(
            [
                "lab8-createdelete",
                "lab8-fourfiles",
            ],
            [test.name for test in tests],
        )
        self.assertEqual(
            [
                ("xv6test --run lab8-createdelete",),
                ("xv6test --run lab8-fourfiles",),
            ],
            [test.commands for test in tests],
        )
        self.assertTrue(all(test.timeout == 180 for test in tests))
        self.assertTrue(
            all(set(RUNNER.GUEST_SUCCESS).issubset(test.expected) for test in tests)
        )


class TimeoutDiagnosticTests(unittest.TestCase):
    """验证超时失败路径会请求 procdump，并有界收集输出。"""

    def test_capture_qemu_procdump_sends_ctrl_p(self) -> None:
        """确保 Ctrl-P 输出被加入稳定诊断标题，且读取超时后立即收敛。"""

        child = mock.Mock()
        child.read_nonblocking.side_effect = [
            "2 sleep cons.read\n",
            RUNNER.pexpect.TIMEOUT("diagnostic complete"),
        ]

        output = RUNNER._capture_qemu_procdump(child)

        child.sendcontrol.assert_called_once_with("p")
        self.assertIn("Ctrl-P procdump", output)
        self.assertIn("2 sleep cons.read", output)
        self.assertEqual(2, child.read_nonblocking.call_count)

    def test_capture_qemu_procdump_marks_empty_output(self) -> None:
        """确保 guest 没有响应 Ctrl-P 时，日志仍明确记录采集结果为空。"""

        child = mock.Mock()
        child.read_nonblocking.side_effect = RUNNER.pexpect.TIMEOUT(
            "no diagnostic output"
        )

        output = RUNNER._capture_qemu_procdump(child)

        self.assertIn("Ctrl-P produced no process output", output)


if __name__ == "__main__":
    unittest.main()
