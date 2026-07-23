#!/usr/bin/env python3
"""验证 changed-file 到 xv6 CI suite 的风险映射。"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


PLANNER_PATH = Path(__file__).with_name("ci_plan.py")
SPEC = importlib.util.spec_from_file_location("xv6_ci_plan", PLANNER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load CI planner from {PLANNER_PATH}")
PLANNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = PLANNER
SPEC.loader.exec_module(PLANNER)


class CiPlanSelectionTests(unittest.TestCase):
    """验证常见改动只选择与风险直接相关的回归集合。"""

    def test_ls_pr_uses_focused_usertests(self) -> None:
        plan = PLANNER.build_ci_plan(
            (
                "Makefile",
                "tests/guest/lstest.c",
                "tests/guest/xv6test.c",
                "user/ls.c",
            )
        )

        self.assertEqual(("usertests-core",), plan.suites)
        self.assertNotIn("usertests-full", plan.suites)
        self.assertNotIn("lab9-bigfile", plan.suites)
        self.assertFalse(plan.run_shell_history)

    def test_filesystem_change_keeps_bigfile_boundary_test(self) -> None:
        plan = PLANNER.build_ci_plan(("kernel/fs.c",))

        self.assertEqual(
            (
                "lab9-symlink",
                "lab10-mmap",
                "usertests-core",
                "lab9-bigfile",
            ),
            plan.suites,
        )

    def test_shell_change_runs_interactive_and_core_regression(self) -> None:
        plan = PLANNER.build_ci_plan(("user/sh.c",))

        self.assertEqual(("usertests-core",), plan.suites)
        self.assertTrue(plan.run_shell_history)

    def test_test_infrastructure_change_uses_safe_pr_fallback(self) -> None:
        plan = PLANNER.build_ci_plan(("tests/run.py",))

        self.assertEqual(PLANNER.PR_FALLBACK_SUITES, plan.suites)
        self.assertNotIn("lab9-bigfile", plan.suites)
        self.assertNotIn("usertests-full", plan.suites)

    def test_unknown_runtime_path_uses_safe_pr_fallback(self) -> None:
        plan = PLANNER.build_ci_plan(("tools/new-helper.py",))

        self.assertEqual(PLANNER.PR_FALLBACK_SUITES, plan.suites)

    def test_mixed_unknown_and_filesystem_change_preserves_bigfile(self) -> None:
        plan = PLANNER.build_ci_plan(("kernel/fs.c", "tools/new-helper.py"))

        self.assertEqual(
            PLANNER.PR_FALLBACK_SUITES + ("lab9-bigfile",),
            plan.suites,
        )

    def test_documentation_only_change_skips_qemu(self) -> None:
        plan = PLANNER.build_ci_plan(("README.md", "docs/testing.markdown"))

        self.assertEqual((), plan.suites)
        self.assertFalse(plan.run_shell_history)

    def test_empty_input_fails_safe_to_pr_fallback(self) -> None:
        plan = PLANNER.build_ci_plan(())

        self.assertEqual(PLANNER.PR_FALLBACK_SUITES, plan.suites)


if __name__ == "__main__":
    unittest.main()
