#!/usr/bin/env python3
"""不启动 QEMU，验证 guest 命令等待逻辑能够快速识别致命输出。"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from collections.abc import Sequence
from pathlib import Path

import pexpect


RUNNER_PATH = Path(__file__).with_name("run.py")
SPEC = importlib.util.spec_from_file_location("xv6_qemu_fail_fast_runner", RUNNER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load regression runner from {RUNNER_PATH}")
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


class FakeSpawn:
    """通过预置匹配事件模拟 `pexpect.spawn.expect_exact`。"""

    def __init__(self, events: Sequence[tuple[object, str, object]]) -> None:
        """保存按调用顺序返回的匹配事件。

        Args:
            events: 每项依次表示待匹配对象、`before` 文本和 `after` 值；
                待匹配对象必须存在于调用方传入的 patterns 中。
        """

        self._events = list(events)
        self.before = ""
        self.after: object = ""
        self.calls: list[tuple[tuple[object, ...], int | None]] = []

    def expect_exact(
        self,
        patterns: Sequence[object],
        timeout: int | None = None,
    ) -> int:
        """消费一个预置事件并返回其在 patterns 中的下标。

        Args:
            patterns: runner 当前等待的 prompt、fatal pattern 或 pexpect sentinel。
            timeout: 本次等待预算；测试只记录该值，不进行真实等待。

        Returns:
            预置匹配对象在 patterns 中的下标。
        """

        if not self._events:
            raise AssertionError("unexpected expect_exact call")
        matched, self.before, self.after = self._events.pop(0)
        pattern_tuple = tuple(patterns)
        self.calls.append((pattern_tuple, timeout))
        return pattern_tuple.index(matched)


class QemuFastFailTests(unittest.TestCase):
    """验证 prompt、fatal、EOF 与 timeout 四类命令结束状态。"""

    def test_prompt_returns_success_output(self) -> None:
        """shell prompt 应返回捕获输出且不产生失败原因。"""

        child = FakeSpawn(((RUNNER.QEMU_SHELL_PROMPT, "done\r\n", "$ "),))

        result = RUNNER._wait_for_qemu_command(child)

        self.assertEqual("done\r\n", result.output)
        self.assertIsNone(result.failure)
        self.assertEqual(1, len(child.calls))

    def test_fatal_outputs_fail_immediately_and_capture_full_line(self) -> None:
        """panic 与 kerneltrap 均应在一秒行尾补采后立即返回明确失败。"""

        for fatal in RUNNER.QEMU_FATAL_OUTPUTS:
            with self.subTest(fatal=fatal):
                child = FakeSpawn(
                    (
                        (fatal, "test twochildren: ", fatal),
                        ("\n", " acquire", "\n"),
                    )
                )

                result = RUNNER._wait_for_qemu_command(child)

                self.assertEqual(f"test twochildren: {fatal} acquire\n", result.output)
                self.assertEqual(f"matched fatal output: {fatal}", result.failure)
                self.assertEqual(2, len(child.calls))
                self.assertEqual(1, child.calls[1][1])

    def test_eof_returns_distinct_failure(self) -> None:
        """QEMU 提前退出应与普通 timeout 使用不同失败原因。"""

        child = FakeSpawn(((pexpect.EOF, "partial output", pexpect.EOF),))

        result = RUNNER._wait_for_qemu_command(child)

        self.assertEqual("partial output", result.output)
        self.assertEqual("QEMU exited before returning to the shell", result.failure)

    def test_timeout_returns_distinct_failure(self) -> None:
        """没有 prompt 或 fatal 输出时应保留既有 timeout 语义。"""

        child = FakeSpawn(((pexpect.TIMEOUT, "still running", pexpect.TIMEOUT),))

        result = RUNNER._wait_for_qemu_command(child)

        self.assertEqual("still running", result.output)
        self.assertEqual(
            "QEMU did not return to the shell before timeout",
            result.failure,
        )


if __name__ == "__main__":
    unittest.main()
