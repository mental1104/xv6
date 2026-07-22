#!/usr/bin/env python3
"""通过真实 QEMU 串口输入验证 xv6 Shell 行编辑与会话历史。"""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import TextIO

import pexpect

REPO_ROOT = Path(__file__).resolve().parents[1]
RESULT_ROOT = REPO_ROOT / "test-results"
TRANSCRIPT_PATH = RESULT_ROOT / "shell-history-interactive.log"
PROMPT = "$ "


class ShellHistoryFailure(RuntimeError):
    """表示交互式 Shell 行为与验收契约不一致。"""


def start_qemu(cpus: int, transcript: TextIO) -> pexpect.spawn:
    """启动独立 snapshot QEMU 并等待首个 Shell 提示符。

    Args:
        cpus: QEMU 虚拟 CPU 数量，必须至少为 1。
        transcript: 持续接收 QEMU 串口输出的文本文件。

    Returns:
        已进入 xv6 Shell 的 pexpect 子进程，调用者负责关闭。
    """

    command = f"make -s --no-print-directory qemu CPUS={cpus} QEMUEXTRA=-snapshot"
    child = pexpect.spawn(
        "/bin/bash",
        ["-lc", command],
        cwd=str(REPO_ROOT),
        encoding="utf-8",
        codec_errors="replace",
        timeout=120,
    )
    child.logfile_read = transcript
    child.expect_exact(PROMPT)
    return child


def stop_qemu(child: pexpect.spawn) -> None:
    """通过 QEMU monitor 快捷键退出实例，失败时强制终止。

    Args:
        child: start_qemu() 返回的活动子进程。
    """

    if not child.isalive():
        return
    child.sendcontrol("a")
    child.send("x")
    try:
        child.expect(pexpect.EOF, timeout=5)
    except (pexpect.TIMEOUT, pexpect.EOF):
        child.terminate(force=True)


def submit(child: pexpect.spawn, text: str, timeout: int = 30) -> str:
    """向当前提示符提交一条命令并返回下一提示符前的原始输出。

    Args:
        child: 已处于 Shell 提示符的 QEMU 子进程。
        text: 不含 Enter 的命令文本。
        timeout: 等待下一提示符的秒数。

    Returns:
        从命令输入到下一提示符之间捕获的原始终端输出。
    """

    child.timeout = timeout
    child.send(text)
    child.send("\r")
    child.expect_exact(PROMPT)
    return child.before or ""


def require(pattern: str, output: str, message: str) -> None:
    """要求正则模式出现在输出中，否则抛出带上下文的失败。

    Args:
        pattern: 使用 MULTILINE 与 DOTALL 匹配的正则表达式。
        output: 待检查的终端输出。
        message: 失败时说明被破坏的行为。
    """

    if re.search(pattern, output, re.MULTILINE | re.DOTALL) is None:
        raise ShellHistoryFailure(f"{message}; pattern={pattern!r}; output={output!r}")


def run_checks(child: pexpect.spawn) -> None:
    """执行历史、方向键、编辑边界及兼容性验收。

    Args:
        child: 已进入全新登录 Shell 的 QEMU 子进程。
    """

    output = submit(child, "echo first")
    require(r"\bfirst\b", output, "首条普通命令未执行")

    child.send("echo draft")
    child.send("\x1b[A")
    child.send("\x1b[B")
    child.send("\r")
    child.expect_exact(PROMPT)
    require(r"\bdraft\b", child.before or "", "下键未恢复进入浏览前的 draft")

    output = submit(child, "history")
    require(r"1 echo first", output, "history 缺少第一条命令")
    require(r"2 echo draft", output, "history 缺少 draft 命令")
    require(r"3 history", output, "history 未包含自身")

    child.send("echo 123456789")
    child.send("\x1b[A")
    child.send("\r")
    child.expect_exact(PROMPT)
    require(r"3 history", child.before or "", "长草稿替换为短历史项后执行内容错误")

    child.send("echo ax")
    child.send("\x7f")
    child.send("b\r")
    child.expect_exact(PROMPT)
    require(r"\bab\b", child.before or "", "Backspace 未删除一个字符")

    child.send("echo wrong")
    child.sendcontrol("u")
    child.send("echo cleared\r")
    child.expect_exact(PROMPT)
    require(r"\bcleared\b", child.before or "", "Ctrl-U 未清空整行")

    child.send("echo esc")
    child.send("\x1b[Z")
    child.send("ape\r")
    child.expect_exact(PROMPT)
    require(r"\bescape\b", child.before or "", "未知 Escape 序列破坏后续输入")

    child.send("x" * 120)
    child.sendcontrol("u")
    child.send("echo max-ok\r")
    child.expect_exact(PROMPT)
    require(r"\bmax-ok\b", child.before or "", "缓冲区满后 Ctrl-U 或 Enter 失效")

    child.send("consolelinetest\r")
    child.expect_exact("consolelinetest: ready")
    child.send("ab\x7fc\r")
    child.expect_exact("consolelinetest: OK")
    child.expect_exact(PROMPT)

    # PR #49 的 jobctl 用 pipe 驱动子 Shell，同时覆盖非 console stdin fallback。
    output = submit(child, "usertests jobctl", timeout=120)
    require(r"ALL TESTS PASSED", output, "PR #49 pipe-driven jobctl 回归失败")

    child.send("echo ctrl-d")
    child.sendcontrol("d")
    child.expect_exact(PROMPT)
    require(r"\bctrl-d\b", child.before or "", "非空行 Ctrl-D 未提交当前输入")

    child.sendcontrol("p")
    child.send("echo ctrlp-ok\r")
    child.expect_exact(PROMPT)
    require(r"\bsh\b.*\bctrlp-ok\b", child.before or "", "Ctrl-P 未保留或 Shell 未继续读取")

    child.sendcontrol("d")
    child.expect_exact("init: starting sh")
    child.expect_exact(PROMPT)


def parse_args() -> argparse.Namespace:
    """解析 QEMU CPU 数量。"""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, default=3, help="QEMU CPU count")
    return parser.parse_args()


def main() -> int:
    """运行全部交互检查，并通过进程退出状态报告结果。"""

    args = parse_args()
    if args.cpus < 1:
        raise SystemExit("--cpus must be at least 1")

    RESULT_ROOT.mkdir(parents=True, exist_ok=True)
    with TRANSCRIPT_PATH.open("w", encoding="utf-8", errors="replace") as transcript:
        child = start_qemu(args.cpus, transcript)
        try:
            run_checks(child)
        except Exception as exc:
            transcript.write(f"\nHOST FAILURE: {exc!r}\n")
            transcript.flush()
            raise
        finally:
            stop_qemu(child)
    print(f"PASS shell-history-interactive ({TRANSCRIPT_PATH.relative_to(REPO_ROOT)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
