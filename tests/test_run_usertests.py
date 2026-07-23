#!/usr/bin/env python3
"""不启动 QEMU，验证逐项 usertests runner 的 completion 协议和失败留档。"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from unittest import mock


RUNNER_PATH = Path(__file__).with_name("run_usertests.py")
SPEC = importlib.util.spec_from_file_location("xv6_usertests_runner", RUNNER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load usertests runner from {RUNNER_PATH}")
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


class CommandOutputTests(unittest.TestCase):
    """验证 guest completion 状态优先于泛化的成功标记和拒绝模式。"""

    def test_successful_completion_passes(self) -> None:
        """status=0、业务成功文本和无拒绝模式时应通过。"""

        RUNNER._assert_command_output(
            "xv6test --usertest reparent2",
            "ALL TESTS PASSED\nXV6TEST done status=0\n",
        )

    def test_nonzero_completion_reports_status(self) -> None:
        """guest 明确返回非零状态时应保留该状态，而不是报缺少成功标记。"""

        with self.assertRaisesRegex(
            RUNNER.RUNNER.TestFailure,
            r"XV6TEST completed with status=1",
        ):
            RUNNER._assert_command_output(
                "xv6test --usertest reparent2",
                "FAILED -- lost some free pages\nXV6TEST done status=1\n",
            )

    def test_missing_completion_marker_is_distinct(self) -> None:
        """没有任何 done marker 时才报告 completion marker 缺失。"""

        with self.assertRaisesRegex(
            RUNNER.RUNNER.TestFailure,
            r"missing XV6TEST completion marker",
        ):
            RUNNER._assert_command_output(
                "xv6test --usertest reparent2",
                "usertests starting\n",
            )


class FailureLogTests(unittest.TestCase):
    """验证输出断言失败与 panic/timeout 一样会保存累计 guest 日志。"""

    class FakeChild:
        """提供 run() 所需的最小 pexpect 子进程接口。"""

        before = "boot output"
        timeout = 0

        def sendline(self, command: str) -> None:
            """记录命令发送动作；单元测试不需要真实终端副作用。"""

            self.command = command

    def test_assertion_failure_writes_log_before_raising(self) -> None:
        """status=1 路径应把当前命令输出写入稳定日志后再抛错。"""

        child = self.FakeChild()
        result = RUNNER.RUNNER.QemuCommandResult(
            output="FAILED -- lost pages\nXV6TEST done status=1\n",
        )
        log_path = Path("/tmp/usertests-full.log")

        with (
            mock.patch.object(RUNNER, "_commands", return_value=("xv6test test",)),
            mock.patch.object(RUNNER.RUNNER, "_start_qemu", return_value=child),
            mock.patch.object(
                RUNNER.RUNNER,
                "_wait_for_qemu_command",
                return_value=result,
            ),
            mock.patch.object(RUNNER.RUNNER, "_write_log", return_value=log_path) as write_log,
            mock.patch.object(RUNNER.RUNNER, "_stop_qemu"),
        ):
            with self.assertRaisesRegex(
                RUNNER.RUNNER.TestFailure,
                r"status=1; log: /tmp/usertests-full.log",
            ):
                RUNNER.run(1)

        write_log.assert_called_once()
        self.assertIn("XV6TEST done status=1", write_log.call_args.args[2])


if __name__ == "__main__":
    unittest.main()
