#!/usr/bin/env python3
"""根据 Pull Request 的 changed files 生成最小且安全的 xv6 CI 回归计划。"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import PurePosixPath
from typing import Iterable, Sequence


# 未识别改动仍执行除 lab9-bigfile 外的现有 PR 回归集，避免选择器漏配后静默降级。
PR_FALLBACK_SUITES = (
    "lab-basic",
    "lab-vm",
    "lab7-thread",
    "lab8-locks",
    "lab9-symlink",
    "lab10-mmap",
    "usertests-core",
)
FILESYSTEM_SUITES = (
    "lab9-symlink",
    "lab10-mmap",
    "usertests-core",
    "lab9-bigfile",
)
VM_SUITES = ("lab-vm", "usertests-core")
CONCURRENCY_SUITES = ("lab7-thread", "lab8-locks", "usertests-core")
BASIC_SUITES = ("lab-basic", "usertests-core")
USERSPACE_SUITES = ("usertests-core",)

DOCUMENT_SUFFIXES = (".md", ".markdown")
TEST_INFRASTRUCTURE_PATHS = {
    ".github/workflows/ci.yml",
    "tests/ci_plan.py",
    "tests/run.py",
    "tests/test_ci_plan.py",
    "tests/test_runner.py",
}
SHELL_HISTORY_PATHS = {
    "user/sh.c",
    "tests/guest/consolelinetest.c",
    "tests/guest/historytest.c",
    "tests/shell_history_interactive.py",
}
FILESYSTEM_PATHS = {
    "kernel/bio.c",
    "kernel/file.c",
    "kernel/fs.c",
    "kernel/fs.h",
    "kernel/log.c",
    "kernel/sysfile.c",
    "kernel/virtio_disk.c",
    "mkfs/mkfs.c",
    "tests/guest/bigfile.c",
    "tests/guest/symlinktest.c",
}
MMAP_PATHS = {
    "kernel/vma.c",
    "kernel/vma.h",
    "tests/guest/mmaptest.c",
}
VM_PATHS = {
    "kernel/exec.c",
    "kernel/kalloc.c",
    "kernel/memlayout.h",
    "kernel/memviz.c",
    "kernel/riscv.h",
    "kernel/sysmemviz.c",
    "kernel/trap.c",
    "kernel/vm.c",
    "kernel/vmcopyin.c",
    "tests/guest/cowtest.c",
    "tests/guest/lazytests.c",
    "tests/guest/memviztest.c",
    "tests/guest/vaaccesstest.c",
}
CONCURRENCY_PATHS = {
    "kernel/proc.c",
    "kernel/sleeplock.c",
    "kernel/spinlock.c",
    "kernel/swtch.S",
    "tests/guest/uthreadtest.c",
    "tests/host/barrier.c",
    "tests/host/ph.c",
}
BASIC_PATHS = {
    "kernel/syscall.c",
    "kernel/syscall.h",
    "kernel/sysproc.c",
    "tests/guest/lab1test.c",
    "tests/guest/sysinfotest.c",
    "tests/guest/tracemasktest.c",
    "tests/guest/tracesmoke.c",
}


@dataclass(frozen=True)
class CiPlan:
    """描述 PR 需要运行的 QEMU suite 与额外 shell 交互检查。"""

    suites: tuple[str, ...]
    run_shell_history: bool = False


def _normalize_path(path: str) -> str:
    """将 Git 输出的相对路径规范为无前导 ``./`` 的 POSIX 路径。

    Args:
        path: ``git diff --name-only`` 输出的一行路径，允许包含首尾空白。

    Returns:
        规范化路径；空白输入返回空字符串。
    """

    stripped = path.strip()
    if not stripped:
        return ""
    normalized = PurePosixPath(stripped).as_posix()
    return normalized.removeprefix("./")


def _deduplicate(values: Iterable[str]) -> tuple[str, ...]:
    """按首次出现顺序去重 suite 名称。

    Args:
        values: 按风险规则累积的 suite 名称序列。

    Returns:
        稳定有序且不含重复项的 suite 元组。
    """

    result: list[str] = []
    seen: set[str] = set()
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        result.append(value)
    return tuple(result)


def _is_documentation_path(path: str) -> bool:
    """判断路径是否只影响不会进入 xv6 镜像的文档或许可证文件。

    Args:
        path: 已规范化的仓库相对路径。

    Returns:
        文档或许可证路径返回 True；其他路径返回 False。
    """

    lower_path = path.lower()
    return lower_path.endswith(DOCUMENT_SUFFIXES) or lower_path.startswith("license")


def build_ci_plan(changed_paths: Iterable[str]) -> CiPlan:
    """根据 changed files 选择覆盖相关风险的最小回归集合。

    Args:
        changed_paths: PR 相对 base 分支修改的仓库路径。空行会被忽略。

    Returns:
        QEMU suite 与 shell history 交互检查组成的不可变计划。文档-only
        变更返回空 suite；没有任何有效路径时按未知改动回退到完整 PR 集。
    """

    paths = tuple(
        normalized
        for raw_path in changed_paths
        if (normalized := _normalize_path(raw_path))
    )
    if not paths:
        return CiPlan(PR_FALLBACK_SUITES)

    selected: list[str] = []
    fallback_required = False
    runtime_path_seen = False
    run_shell_history = False

    for path in paths:
        if _is_documentation_path(path):
            continue

        runtime_path_seen = True
        matched = False

        # 测试编排自身变化可能影响所有映射，必须使用较宽的 PR 回归集兜底。
        if path in TEST_INFRASTRUCTURE_PATHS:
            fallback_required = True
            matched = True

        if path in SHELL_HISTORY_PATHS:
            selected.extend(USERSPACE_SUITES)
            run_shell_history = True
            matched = True

        if path in FILESYSTEM_PATHS:
            selected.extend(FILESYSTEM_SUITES)
            matched = True

        if path in MMAP_PATHS:
            selected.extend(("lab10-mmap", "usertests-core"))
            matched = True

        if path in VM_PATHS:
            selected.extend(VM_SUITES)
            matched = True

        if path in CONCURRENCY_PATHS:
            selected.extend(CONCURRENCY_SUITES)
            matched = True

        if path in BASIC_PATHS:
            selected.extend(BASIC_SUITES)
            matched = True

        # 用户程序、guest 断言与构建清单通过 focused usertests 验证即可；完整
        # 编译已经在工作流中单独执行，不因 Makefile 变化强制跑全部 Lab。
        if path == "Makefile" or path == "tests/guest/xv6test.c":
            selected.extend(USERSPACE_SUITES)
            matched = True
        elif path.startswith("user/") or path.startswith("tests/guest/"):
            selected.extend(USERSPACE_SUITES)
            matched = True
        elif path.startswith("tests/host/"):
            selected.append("lab7-thread")
            matched = True

        if not matched:
            fallback_required = True

    if not runtime_path_seen:
        return CiPlan(())

    if fallback_required:
        # 未识别改动采用现有 PR 范围；若同时命中文件系统关键路径，仍保留
        # lab9-bigfile，避免较宽 fallback 反而丢掉慢速边界验证。
        fallback = list(PR_FALLBACK_SUITES)
        if "lab9-bigfile" in selected:
            fallback.append("lab9-bigfile")
        selected = fallback

    if not selected:
        selected.extend(PR_FALLBACK_SUITES)

    return CiPlan(
        suites=_deduplicate(selected),
        run_shell_history=run_shell_history,
    )


def _read_paths(arguments: Sequence[str]) -> tuple[str, ...]:
    """读取命令行路径，未提供参数时从标准输入逐行读取。

    Args:
        arguments: 命令行位置参数中的 changed files。

    Returns:
        原始路径元组，后续由 ``build_ci_plan`` 统一规范化。
    """

    if arguments:
        return tuple(arguments)
    return tuple(sys.stdin.read().splitlines())


def parse_args() -> argparse.Namespace:
    """解析可选 changed-file 位置参数。"""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths",
        nargs="*",
        help="changed file paths; omitted arguments are read from standard input",
    )
    return parser.parse_args()


def main() -> int:
    """输出可直接追加到 ``GITHUB_OUTPUT`` 的测试计划键值对。

    Returns:
        成功生成计划时返回 0。路径映射不会因未知文件失败，而是回退到安全集。
    """

    args = parse_args()
    plan = build_ci_plan(_read_paths(args.paths))
    print(f"suites={','.join(plan.suites)}")
    print(f"shell_history={'true' if plan.run_shell_history else 'false'}")
    print(
        "Selected CI plan: "
        f"suites={plan.suites or ('build-only',)}, "
        f"shell_history={plan.run_shell_history}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
