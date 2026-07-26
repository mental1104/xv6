#!/usr/bin/env python3
"""不启动 QEMU，验证日志崩溃恢复编排的静态契约。"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


RUNNER_PATH = Path(__file__).with_name("run.py")
SPEC = importlib.util.spec_from_file_location("xv6_log_recovery_runner", RUNNER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load regression runner from {RUNNER_PATH}")
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


class LogRecoveryScenarioTests(unittest.TestCase):
    """验证三阶段矩阵、CPU 证据和持久磁盘命令不会静默退化。"""

    def test_matrix_covers_required_commit_boundaries(self) -> None:
        self.assertEqual(
            ("before-commit", "after-commit", "during-install"),
            tuple(scenario.name for scenario in RUNNER.LOG_RECOVERY_SCENARIOS),
        )
        self.assertEqual(
            (False, True, True),
            tuple(
                scenario.recovery_expected
                for scenario in RUNNER.LOG_RECOVERY_SCENARIOS
            ),
        )

    def test_crash_markers_are_phase_specific(self) -> None:
        markers = tuple(
            scenario.crash_marker for scenario in RUNNER.LOG_RECOVERY_SCENARIOS
        )
        self.assertEqual(len(markers), len(set(markers)))
        for scenario in RUNNER.LOG_RECOVERY_SCENARIOS:
            self.assertIn(scenario.name, scenario.crash_marker)

    def test_persistent_qemu_command_uses_explicit_image_without_snapshot(self) -> None:
        image = Path("test-results/log-recovery.img")
        command = RUNNER._build_qemu_command(3, fsimg=image, snapshot=False)

        self.assertIn("CPUS=3", command)
        self.assertIn(f"FSIMG={image}", command)
        self.assertNotIn("QEMUEXTRA=-snapshot", command)

    def test_normal_qemu_command_keeps_snapshot_isolation(self) -> None:
        command = RUNNER._build_qemu_command(3)

        self.assertIn("QEMUEXTRA=-snapshot", command)
        self.assertNotIn("FSIMG=", command)

    def test_multicore_request_adds_single_core_evidence_once(self) -> None:
        self.assertEqual((3, 1), RUNNER._crash_cpu_variants(3))
        self.assertEqual((1,), RUNNER._crash_cpu_variants(1))

    def test_storage_suite_owns_recovery_scenarios(self) -> None:
        suite = RUNNER.SUITES["lab9-bigfile"]

        self.assertEqual(RUNNER.LOG_RECOVERY_SCENARIOS, suite.crash_scenarios)
        self.assertIn("logcrash-api", tuple(test.name for test in suite.tests))


if __name__ == "__main__":
    unittest.main()
