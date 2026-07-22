#!/usr/bin/env python3
"""Unit tests for the xv6 regression grader itself.

These tests do not start QEMU. They validate suite composition, output
matching, failure detection, and helper behavior before the grader is used to
judge the kernel.
"""

from __future__ import annotations

import importlib.util
import re
import sys
import unittest
from pathlib import Path


RUNNER_PATH = Path(__file__).with_name("run.py")
SPEC = importlib.util.spec_from_file_location("xv6_regression_runner", RUNNER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load regression runner from {RUNNER_PATH}")
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


class OutputMatchingTests(unittest.TestCase):
    def test_expected_and_counted_patterns_pass(self) -> None:
        test = RUNNER.TestCase(
            name="sample",
            commands=("sample",),
            expected=(r"^done$",),
            counted=(RUNNER.CountExpectation(r"^item \d+$", 2),),
        )

        RUNNER._assert_output(test, "item 1\nitem 2\ndone\n")

    def test_missing_expected_pattern_fails(self) -> None:
        test = RUNNER.TestCase(
            name="sample",
            commands=("sample",),
            expected=(r"^done$",),
        )

        with self.assertRaisesRegex(RUNNER.TestFailure, "missing expected pattern"):
            RUNNER._assert_output(test, "not done\n")

    def test_rejected_kernel_failure_fails(self) -> None:
        test = RUNNER.TestCase(name="sample", commands=("sample",))

        with self.assertRaisesRegex(RUNNER.TestFailure, "matched rejected pattern"):
            RUNNER._assert_output(test, "panic: broken invariant\n")

    def test_counted_pattern_requires_minimum(self) -> None:
        test = RUNNER.TestCase(
            name="sample",
            commands=("sample",),
            counted=(RUNNER.CountExpectation(r"^frame$", 3),),
        )

        with self.assertRaisesRegex(RUNNER.TestFailure, "expected at least 3"):
            RUNNER._assert_output(test, "frame\nframe\n")

    def test_guest_protocol_requires_successful_done_marker(self) -> None:
        """guest 测试必须输出 status=0 的稳定结束行，其他自然语言不能代替它。"""
        test = RUNNER.TestCase(
            name="guest",
            commands=("xv6test --group lab3",),
            expected=RUNNER.GUEST_SUCCESS,
        )

        RUNNER._assert_output(test, "XV6TEST summary selected=4 passed=4 failed=0\nXV6TEST done status=0\n")
        with self.assertRaisesRegex(RUNNER.TestFailure, "missing expected pattern"):
            RUNNER._assert_output(test, "XV6TEST done status=1\n")


class SuiteCompositionTests(unittest.TestCase):
    def test_all_include_references_exist(self) -> None:
        for suite in RUNNER.SUITES.values():
            for included in suite.includes:
                self.assertIn(included, RUNNER.SUITES, f"{suite.name} includes unknown suite")

    def test_atomic_suites_have_tests(self) -> None:
        for suite in RUNNER.SUITES.values():
            if not suite.includes:
                self.assertTrue(suite.tests, f"atomic suite {suite.name} has no tests")

    def test_suite_names_match_dictionary_keys(self) -> None:
        for key, suite in RUNNER.SUITES.items():
            self.assertEqual(key, suite.name)

    def test_pr_suite_covers_lab1_through_lab10(self) -> None:
        expanded = RUNNER._deduplicate(RUNNER._expand_suite("pr"))
        expected = {
            "lab-basic",
            "lab-vm",
            "lab7-thread",
            "lab8-locks",
            "lab9-bigfile",
            "lab9-symlink",
            "lab10-mmap",
            "usertests-core",
        }
        self.assertEqual(expected, set(expanded))

    def test_full_suite_uses_complete_usertests(self) -> None:
        expanded = RUNNER._deduplicate(RUNNER._expand_suite("full"))
        self.assertIn("usertests-full", expanded)
        self.assertNotIn("usertests-core", expanded)

    def test_no_duplicate_test_names_inside_atomic_suite(self) -> None:
        for suite in RUNNER.SUITES.values():
            names = [test.name for test in suite.tests]
            self.assertEqual(
                len(names),
                len(set(names)),
                f"suite {suite.name} contains duplicate test names",
            )

    def test_vm_suite_routes_migrated_labs_through_guest_runner(self) -> None:
        """Lab3、Lab5、Lab6 只通过 xv6test group 暴露给宿主机 runner。"""
        suite = RUNNER.SUITES["lab-vm"]
        commands = {test.commands for test in suite.tests}

        self.assertIn(("xv6test --group lab3",), commands)
        self.assertIn(("xv6test --group lab5",), commands)
        self.assertIn(("xv6test --group lab6",), commands)
        self.assertNotIn(("usertests copyin",), commands)
        self.assertNotIn(("lazytests",), commands)
        self.assertNotIn(("cowtest",), commands)

    def test_core_suite_does_not_repeat_migrated_lab3_cases(self) -> None:
        """PR 级 focused usertests 不再重复执行已迁移到 guest group 的 Lab3 用例。"""
        commands = {test.commands for test in RUNNER.SUITES["usertests-core"].tests}

        self.assertNotIn(("usertests copyin",), commands)
        self.assertNotIn(("usertests copyout",), commands)
        self.assertNotIn(("usertests copyinstr1",), commands)

    def test_unknown_suite_is_rejected(self) -> None:
        with self.assertRaises(KeyError):
            RUNNER._expand_suite("does-not-exist")


class HelperTests(unittest.TestCase):
    def test_safe_name_removes_path_and_shell_characters(self) -> None:
        safe = RUNNER._safe_name("lab 1/$(rm -rf)")

        self.assertIsNotNone(re.fullmatch(r"[A-Za-z0-9_.-]+", safe))
        self.assertNotIn("/", safe)
        self.assertNotIn("$", safe)
        self.assertNotIn("(", safe)
        self.assertNotIn(")", safe)


if __name__ == "__main__":
    unittest.main()
