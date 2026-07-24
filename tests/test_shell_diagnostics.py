#!/usr/bin/env python3
"""不启动 QEMU，验证 Shell 命令不存在诊断的源码契约。"""

from __future__ import annotations

import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SHELL_SOURCE_PATH = REPO_ROOT / "user" / "sh.c"


class ShellDiagnosticTests(unittest.TestCase):
    """验证 PATH 搜索失败后的用户可见诊断及控制流顺序。"""

    def test_missing_command_uses_quoted_command_not_found_message(self) -> None:
        """命令不存在时必须用双引号包裹原始命令名。"""

        source = SHELL_SOURCE_PATH.read_text(encoding="utf-8")
        self.assertIn(
            'fprintf(2, "\\\"%s\\\" command not found\\n", program);',
            source,
        )

    def test_failure_exits_before_legacy_shell_core_diagnostic(self) -> None:
        """PATH 候选耗尽后应在进入 shcore.inc 的旧失败输出前结束子进程。"""

        source = SHELL_SOURCE_PATH.read_text(encoding="utf-8")
        exec_position = source.index("execvpe(program, argv, shell_environment);")
        message_position = source.index(
            'fprintf(2, "\\\"%s\\\" command not found\\n", program);',
            exec_position,
        )
        exit_position = source.index("exit(0);", message_position)
        include_position = source.index('#include "user/shcore.inc"', exit_position)

        self.assertLess(exec_position, message_position)
        self.assertLess(message_position, exit_position)
        self.assertLess(exit_position, include_position)


if __name__ == "__main__":
    unittest.main()
