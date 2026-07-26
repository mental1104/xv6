#!/usr/bin/env python3
"""锁模型 CPU 验证矩阵的宿主机单元测试。"""

from __future__ import annotations

import unittest
from unittest.mock import patch

import run_lock_model


class CpuMatrixTests(unittest.TestCase):
    """验证单核固定覆盖和多核去重规则。"""

    def test_requested_single_core_is_not_duplicated(self) -> None:
        self.assertEqual(run_lock_model.cpu_matrix(1), (1,))

    def test_requested_multi_core_runs_after_single_core(self) -> None:
        self.assertEqual(run_lock_model.cpu_matrix(3), (1, 3))

    def test_invalid_cpu_count_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "at least 1"):
            run_lock_model.cpu_matrix(0)


class RunnerInvocationTests(unittest.TestCase):
    """验证每个矩阵元素都会交给 QEMU runner。"""

    @patch("run_lock_model.run_lock_model")
    def test_main_runs_single_and_requested_multi_core(self, mocked_run) -> None:
        self.assertEqual(run_lock_model.main(("--cpus", "3")), 0)
        self.assertEqual(mocked_run.call_count, 2)
        mocked_run.assert_any_call(1)
        mocked_run.assert_any_call(3)


if __name__ == "__main__":
    unittest.main()
