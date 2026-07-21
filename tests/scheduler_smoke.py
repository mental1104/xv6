#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
from pathlib import Path

import pexpect

REPO_ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--policy", required=True)
    parser.add_argument("--cpus", type=int, default=1)
    args = parser.parse_args()

    command = (
        "make -s --no-print-directory qemu "
        f"SCHED_POLICY={args.policy} CPUS={args.cpus} QEMUEXTRA=-snapshot"
    )
    child = pexpect.spawn(
        "/bin/bash",
        ["-lc", command],
        cwd=str(REPO_ROOT),
        encoding="utf-8",
        codec_errors="replace",
        timeout=180,
    )
    output: list[str] = []
    try:
        child.expect_exact("$ ")
        output.append(child.before)
        boot = "".join(output)
        if not re.search(rf"scheduler: {re.escape(args.policy)}", boot):
            raise RuntimeError(f"missing scheduler banner for {args.policy}")

        child.sendline("schedtest verify" if args.cpus == 1 else "schedtest smoke")
        child.expect_exact("$ ")
        output.append(child.before)
        result = "".join(output)
        if f"schedtest: OK policy={args.policy}" not in result:
            raise RuntimeError("schedtest did not report success")
        if "panic:" in result or "kerneltrap" in result:
            raise RuntimeError("kernel failure detected")
        print(result)
        return 0
    finally:
        if child.isalive():
            child.sendcontrol("a")
            child.send("x")
            try:
                child.expect(pexpect.EOF, timeout=5)
            except (pexpect.TIMEOUT, pexpect.EOF):
                child.terminate(force=True)


if __name__ == "__main__":
    raise SystemExit(main())
