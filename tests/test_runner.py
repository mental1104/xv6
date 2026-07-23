#!/usr/bin/env python3
"""不启动 QEMU，验证 xv6 regression runner 的结构和匹配规则。"""

from __future__ import annotations

import importlib.util
import re
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
RUNNER_PATH = Path(__file__).with_name("run.py")
SPEC = importlib.util.spec_from_file_location("xv6_regression_runner", RUNNER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load regression runner from {RUNNER_PATH}")
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)

HOST_SOURCE_NAMES = ("ph.c", "barrier.c")
LEGACY_SOURCE_NAMES = ("grade-lab-util", "gradelib.py")

GUEST_SOURCE_NAMES = (
    "forktest.c",
    "stressfs.c",
    "usertests.c",
    "grind.c",
    "tracemasktest.c",
    "sysinfotest.c",
    "bttest.c",
    "alarmtest.c",
    "lazytests.c",
    "cowtest.c",
    "bigfile.c",
    "symlinktest.c",
    "mmaptest.c",
    "lab1test.c",
    "tracesmoke.c",
    "uthreadtest.c",
    "vaaccesstest.c",
    "addresswindowtest.c",
    "testlib.c",
    "xv6test.c",
)


class OutputMatchingTests(unittest.TestCase):
    """验证 host runner 的基础设施级输出规则。"""

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
        test = RUNNER.TestCase(
            name="guest",
            commands=("xv6test --group lab3",),
            expected=RUNNER.GUEST_SUCCESS,
        )
        RUNNER._assert_output(
            test,
            "XV6TEST summary selected=4 passed=4 failed=0\n"
            "XV6TEST done status=0\n",
        )
        with self.assertRaisesRegex(RUNNER.TestFailure, "missing expected pattern"):
            RUNNER._assert_output(test, "XV6TEST done status=1\n")


class SuiteCompositionTests(unittest.TestCase):
    """验证 suite 图以及 Lab1-Lab10 的 guest-first 路由。"""

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
                f"{suite.name} contains duplicate test names",
            )

    def test_all_qemu_lab_commands_enter_through_xv6test(self) -> None:
        lab_suites = (
            "lab-basic",
            "lab-vm",
            "lab7-thread",
            "lab8-locks",
            "lab9-bigfile",
            "lab9-symlink",
            "lab10-mmap",
            "usertests-core",
            "usertests-full",
        )
        for suite_name in lab_suites:
            for test in RUNNER.SUITES[suite_name].tests:
                if test.host:
                    continue
                self.assertTrue(
                    all(command.startswith("xv6test ") for command in test.commands),
                    f"{suite_name}/{test.name} bypasses xv6test: {test.commands}",
                )
                self.assertTrue(
                    set(RUNNER.GUEST_SUCCESS).issubset(test.expected),
                    f"{suite_name}/{test.name} does not require guest completion",
                )

    def test_lab2_and_lab4_keep_only_hardware_output_checks_on_host(self) -> None:
        lab2 = next(
            test for test in RUNNER.SUITES["lab-basic"].tests
            if test.name == "lab2-guest-tests"
        )
        lab4 = next(
            test for test in RUNNER.SUITES["lab-vm"].tests
            if test.name == "lab4-guest-tests"
        )
        self.assertIn(r"syscall read ->", lab2.expected)
        self.assertEqual((RUNNER.CountExpectation(r"^0x[0-9a-f]+$", 3),), lab4.counted)

    def test_unknown_suite_is_rejected(self) -> None:
        with self.assertRaises(KeyError):
            RUNNER._expand_suite("does-not-exist")


class RepositoryLayoutTests(unittest.TestCase):
    """验证测试源码与 user/kernel 实现目录保持物理分离。"""

    def test_guest_test_sources_live_only_under_tests_guest(self) -> None:
        for name in GUEST_SOURCE_NAMES:
            self.assertTrue((REPO_ROOT / "tests" / "guest" / name).is_file(), name)
            self.assertFalse((REPO_ROOT / "user" / name).exists(), name)
            self.assertFalse((REPO_ROOT / "kernel" / name).exists(), name)

    def test_host_test_sources_live_only_under_tests_host(self) -> None:
        for name in HOST_SOURCE_NAMES:
            self.assertTrue((REPO_ROOT / "tests" / "host" / name).is_file(), name)
            self.assertFalse((REPO_ROOT / "notxv6" / name).exists(), name)
            self.assertFalse((REPO_ROOT / "user" / name).exists(), name)
            self.assertFalse((REPO_ROOT / "kernel" / name).exists(), name)

    def test_legacy_graders_live_only_under_tests_legacy(self) -> None:
        for name in LEGACY_SOURCE_NAMES:
            self.assertTrue((REPO_ROOT / "tests" / "legacy" / name).is_file(), name)
            self.assertFalse((REPO_ROOT / name).exists(), name)

    def test_makefile_builds_tests_from_canonical_directories(self) -> None:
        makefile = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("T=tests/guest", makefile)
        self.assertIn("H=tests/host", makefile)
        self.assertIn("$U/_%: $T/%.o $(ULIB)", makefile)
        self.assertIn("ph: $H/ph.c", makefile)
        self.assertIn("barrier: $H/barrier.c", makefile)
        self.assertIn("-include kernel/*.d user/*.d tests/guest/*.d", makefile)


class HelperTests(unittest.TestCase):
    """验证与日志路径相关的纯 Python helper。"""

    def test_safe_name_removes_path_and_shell_characters(self) -> None:
        safe = RUNNER._safe_name("lab 1/$(rm -rf)")
        self.assertIsNotNone(re.fullmatch(r"[A-Za-z0-9_.-]+", safe))
        self.assertNotIn("/", safe)
        self.assertNotIn("$", safe)
        self.assertNotIn("(", safe)
        self.assertNotIn(")", safe)


if __name__ == "__main__":
    unittest.main()
