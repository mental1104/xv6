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
    """通过预置匹配事件和诊断输出模拟 `pexpect.spawn`。"""

    def __init__(
        self,
        events: Sequence[tuple[object, str, object]],
        diagnostics: Sequence[object] = (),
    ) -> None:
        """保存按调用顺序返回的匹配事件和非阻塞诊断数据。

        Args:
            events: 每项依次表示待匹配对象、`before` 文本和 `after` 值；
                待匹配对象必须存在于调用方传入的 patterns 中。
            diagnostics: `read_nonblocking()` 依次返回或抛出的对象。异常对象会
                被直接抛出；队列耗尽后默认抛出 `pexpect.TIMEOUT`。
        """

        self._events = list(events)
        self._diagnostics = list(diagnostics)
        self.before = ""
        self.after: object = ""
        self.calls: list[tuple[tuple[object, ...], int | None]] = []
        self.controls: list[str] = []

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

    def sendcontrol(self, key: str) -> None:
        """记录 runner 向 guest console 发送的控制键。

        Args:
            key: 不含 `Ctrl-` 前缀的单字符控制键名称。
        """

        self.controls.append(key)

    def read_nonblocking(self, size: int, timeout: float) -> str:
        """返回一段预置诊断输出，或抛出预置的 pexpect 异常。

        Args:
            size: runner 单次允许读取的最大字符数；测试替身只记录接口兼容性。
            timeout: 单次非阻塞读取预算；测试替身不进行真实等待。

        Returns:
            下一段诊断文本。

        Raises:
            pexpect.TIMEOUT: 没有更多诊断数据或预置队列要求结束读取时抛出。
            pexpect.EOF: 预置队列要求模拟 QEMU 退出时抛出。
        """

        del size, timeout
        if not self._diagnostics:
            raise pexpect.TIMEOUT("diagnostic queue exhausted")
        result = self._diagnostics.pop(0)
        if isinstance(result, BaseException):
            raise result
        if not isinstance(result, str):
            raise AssertionError(f"unexpected diagnostic value: {result!r}")
        return result


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
        self.assertEqual([], child.controls)

    def test_timeout_returns_failure_with_bounded_procdump(self) -> None:
        """timeout 应保留失败语义，并附加一次 Ctrl-P 进程状态诊断。"""

        child = FakeSpawn(
            ((pexpect.TIMEOUT, "still running", pexpect.TIMEOUT),),
            diagnostics=(
                "2 sleep cons.read\n",
                pexpect.TIMEOUT("diagnostic complete"),
            ),
        )

        result = RUNNER._wait_for_qemu_command(child)

        self.assertIn("still running", result.output)
        self.assertIn("Ctrl-P procdump", result.output)
        self.assertIn("2 sleep cons.read", result.output)
        self.assertEqual(["p"], child.controls)
        self.assertEqual(
            "QEMU did not return to the shell before timeout",
            result.failure,
        )


if __name__ == "__main__":
    unittest.main()
