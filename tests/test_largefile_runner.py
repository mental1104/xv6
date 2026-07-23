"""Issue #29 大文件 guest-first 布局与宿主机编排自测。"""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


class LargeFileRunnerTests(unittest.TestCase):
    """验证 4 GiB 回归保持显式、隔离且由 guest 承担行为断言。"""

    def test_guest_source_and_make_target_are_registered(self) -> None:
        self.assertTrue((REPO_ROOT / "tests/guest/largefile.c").is_file())
        makefile = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("$U/_largefile", makefile)
        self.assertIn("largefiletest:", makefile)
        self.assertIn("fs-large.img", makefile)
        self.assertIn("mkfs/mkfs_large.c", makefile)

    def test_dedicated_runner_registers_only_explicit_suite(self) -> None:
        runner_path = REPO_ROOT / "tests/run_largefile.py"
        spec = importlib.util.spec_from_file_location("run_largefile", runner_path)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        suite = module.runner.SUITES["largefs-4gib"]
        self.assertEqual(suite.tests[0].commands, ("xv6test --run largefs-4gib",))
        self.assertNotIn("largefs-4gib", module.runner.SUITES["pr"].includes)
        self.assertNotIn("largefs-4gib", module.runner.SUITES["full"].includes)


if __name__ == "__main__":
    unittest.main()
