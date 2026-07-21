#!/usr/bin/env python3
"""Run vmbench on pristine/lazy/cow/main and generate CSV/Markdown/SVG reports."""
from __future__ import annotations

import argparse
import json
import os
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

VARIANTS = {
    "pristine": "concept/21-vmbench-pristine",
    "lazy": "concept/21-vmbench-lazy",
    "cow": "concept/21-vmbench-cow",
    "main": "concept/21-vmbench-main",
}
EXPECTED_RESULTS = 9
TAIL = 16384


def run(command: list[str], cwd: Path, **kwargs):
    return subprocess.run(command, cwd=cwd, check=True, **kwargs)


def git(repo: Path, *args: str) -> str:
    return run(
        ["git", *args], repo, text=True, capture_output=True
    ).stdout.strip()


def tail_file(path: Path) -> str:
    try:
        data = path.read_bytes()
    except OSError as error:
        return f"<cannot read {path}: {error}>"
    return data[-TAIL:].decode(errors="replace") if data else "<empty>"


def prompt_seen(data: bytes) -> bool:
    return b"$ " in data or data.rstrip().endswith(b"$")


def qemu_args(worktree: Path) -> list[str]:
    qemu = shutil.which("qemu-system-riscv64")
    if not qemu:
        raise RuntimeError("qemu-system-riscv64 not found")
    return [
        qemu,
        "-machine", "virt",
        "-bios", "none",
        "-kernel", str(worktree / "kernel/kernel"),
        "-m", "128M",
        "-smp", "1",
        "-display", "none",
        "-monitor", "none",
        "-chardev", "stdio,id=xv6serial,signal=off",
        "-serial", "chardev:xv6serial",
        "-drive", f"file={worktree / 'fs.img'},if=none,format=raw,id=x0",
        "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
    ]


def stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5)
    except ProcessLookupError:
        return
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            return
        process.wait(timeout=5)


def wait_output(
    process: subprocess.Popen[bytes],
    serial_log,
    host_log: Path,
    predicate,
    timeout: int,
    description: str,
) -> bytes:
    assert process.stdout is not None
    fd = process.stdout.fileno()
    deadline = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"QEMU exited before {description}; exit={process.returncode}\n"
                f"--- xv6 tail ---\n{bytes(data[-TAIL:]).decode(errors='replace')}\n"
                f"--- QEMU tail ---\n{tail_file(host_log)}"
            )
        ready, _, _ = select.select([fd], [], [], 0.5)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except BlockingIOError:
            continue
        if not chunk:
            continue
        serial_log.write(chunk)
        serial_log.flush()
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        data.extend(chunk)
        if len(data) > 2_000_000:
            del data[:1_000_000]
        if predicate(bytes(data)):
            return bytes(data)
    raise TimeoutError(
        f"timed out after {timeout}s waiting for {description}\n"
        f"--- xv6 tail ---\n{bytes(data[-TAIL:]).decode(errors='replace')}\n"
        f"--- QEMU tail ---\n{tail_file(host_log)}"
    )


def send(process: subprocess.Popen[bytes], command: str) -> None:
    assert process.stdin is not None
    process.stdin.write((command + "\n").encode())
    process.stdin.flush()


def run_qemu(
    worktree: Path,
    serial_log_path: Path,
    host_log_path: Path,
    pages: int,
    timeout: int,
    regressions: list[str],
) -> None:
    serial_log_path.parent.mkdir(parents=True, exist_ok=True)
    with host_log_path.open("wb") as host_log:
        process = subprocess.Popen(
            qemu_args(worktree),
            cwd=worktree,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=host_log,
            bufsize=0,
            start_new_session=True,
        )
        try:
            assert process.stdout is not None
            os.set_blocking(process.stdout.fileno(), False)
            with serial_log_path.open("wb") as serial_log:
                wait_output(
                    process, serial_log, host_log_path,
                    prompt_seen, timeout, "xv6 shell prompt",
                )
                send(process, f"vmbench all {pages}")
                result = wait_output(
                    process, serial_log, host_log_path,
                    lambda x: x.count(b"VMRESULT ") >= EXPECTED_RESULTS
                    and prompt_seen(x),
                    timeout, f"{EXPECTED_RESULTS} VMRESULT lines",
                )
                if b"VMFAILED " in result or b"VMERROR " in result:
                    raise RuntimeError("vmbench reported VMFAILED/VMERROR")
                for command in regressions:
                    send(process, command)
                    result = wait_output(
                        process, serial_log, host_log_path,
                        prompt_seen, timeout, f"completion of {command}",
                    )
                    marker = (
                        b"ALL COW TESTS PASSED"
                        if command == "cowtest" else b"ALL TESTS PASSED"
                    )
                    if marker not in result:
                        raise RuntimeError(f"{command} did not print {marker!r}")
        finally:
            if process.stdin:
                process.stdin.close()
            stop_process(process)


def regression_commands(variant: str, enabled: bool) -> list[str]:
    if not enabled:
        return []
    commands = []
    if variant in ("lazy", "main"):
        commands.append("lazytests")
    if variant in ("cow", "main"):
        commands.append("cowtest")
    return [*commands, "usertests"]


def add_worktree(repo: Path, path: Path, ref: str) -> None:
    if path.exists():
        run(["git", "worktree", "remove", "--force", str(path)], repo)
    run(["git", "worktree", "add", "--detach", str(path), ref], repo)


def remove_worktree(repo: Path, path: Path) -> None:
    if path.exists():
        subprocess.run(
            ["git", "worktree", "remove", "--force", str(path)],
            cwd=repo, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            check=False,
        )


def write_manifest(output: Path, pages: int, regression: bool, records, error):
    output.mkdir(parents=True, exist_ok=True)
    payload = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "pages": pages,
        "cpus": 1,
        "with_regression": regression,
        "error": error,
        "records": records,
    }
    (output / "manifest.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--remote", default="origin")
    parser.add_argument("--pages", type=int, default=4096)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--with-regression", action="store_true")
    parser.add_argument("--keep-worktrees", action="store_true")
    parser.add_argument("--skip-fetch", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.pages <= 0:
        parser.error("--pages must be positive")

    output = None
    records = []
    try:
        for command in ("git", "make", "python3"):
            if not shutil.which(command):
                raise RuntimeError(f"{command} not found")
        if not args.dry_run and not shutil.which("qemu-system-riscv64"):
            raise RuntimeError("qemu-system-riscv64 not found")

        repo = Path(git(args.repo.resolve(), "rev-parse", "--show-toplevel"))
        output = (
            args.output
            or repo.parent / f"{repo.name}-vmbench-results"
            / datetime.now().strftime("%Y%m%d-%H%M%S")
        ).resolve()
        refs = {name: f"{args.remote}/{branch}" for name, branch in VARIANTS.items()}
        print("VM benchmark plan:")
        for name, ref in refs.items():
            print(f"  {name:8} <- {ref}")
        print(f"  pages     = {args.pages}")
        print(f"  output    = {output}")
        print(f"  regression= {args.with_regression}")
        if args.dry_run:
            return 0

        output.mkdir(parents=True, exist_ok=True)
        if not args.skip_fetch:
            run(["git", "fetch", "--prune", args.remote], repo)
        worktree_root = Path(tempfile.mkdtemp(prefix="xv6-vmbench-worktrees-"))
        worktrees = []
        error = None
        try:
            for name, branch in VARIANTS.items():
                worktree = worktree_root / name
                print(f"\n==> [{name}] checkout {refs[name]}")
                add_worktree(repo, worktree, refs[name])
                worktrees.append(worktree)
                record = {
                    "variant": name,
                    "branch": branch,
                    "commit": git(worktree, "rev-parse", "HEAD"),
                    "build_log": str(output / "build" / f"{name}.log"),
                    "qemu_log": str(output / "logs" / f"{name}.log"),
                    "qemu_host_log": str(output / "logs" / f"{name}.host.log"),
                    "build_ok": False,
                    "qemu_ok": False,
                }
                records.append(record)
                print(f"==> [{name}] make clean && make kernel/kernel fs.img")
                build_log = Path(record["build_log"])
                build_log.parent.mkdir(parents=True, exist_ok=True)
                with build_log.open("w") as stream:
                    run(["make", "clean"], worktree, text=True,
                        stdout=stream, stderr=subprocess.STDOUT)
                    run(["make", "kernel/kernel", "fs.img"], worktree,
                        text=True, stdout=stream, stderr=subprocess.STDOUT)
                for artifact in (worktree / "kernel/kernel", worktree / "fs.img"):
                    if not artifact.is_file():
                        raise RuntimeError(f"missing build artifact: {artifact}")
                record["build_ok"] = True
                print(f"==> [{name}] QEMU stdio: vmbench all {args.pages}")
                run_qemu(
                    worktree,
                    Path(record["qemu_log"]),
                    Path(record["qemu_host_log"]),
                    args.pages,
                    args.timeout,
                    regression_commands(name, args.with_regression),
                )
                record["qemu_ok"] = True

            compare = worktree_root / "main/tools/vmbench_compare.py"
            command = [
                sys.executable, str(compare),
                *(f"{name}={output / 'logs' / (name + '.log')}" for name in VARIANTS),
                "--csv", str(output / "vmbench-results.csv"),
                "--markdown", str(output / "vmbench-results.md"),
                "--svg-dir", str(output / "charts"),
            ]
            print("\n==> compare logs and render SVG charts")
            with (output / "compare.stdout.md").open("w") as stream:
                run(command, worktree_root / "main", text=True,
                    stdout=stream, stderr=subprocess.STDOUT)
        except Exception as caught:
            error = str(caught)
            raise
        finally:
            write_manifest(output, args.pages, args.with_regression, records, error)
            if not args.keep_worktrees:
                for worktree in reversed(worktrees):
                    remove_worktree(repo, worktree)
                subprocess.run(["git", "worktree", "prune"], cwd=repo, check=False)
                shutil.rmtree(worktree_root, ignore_errors=True)

        print(f"\nDone. Results: {output}")
        return 0
    except (OSError, RuntimeError, TimeoutError, subprocess.CalledProcessError) as error:
        print(f"vmbench_run_all: {error}", file=sys.stderr)
        if output:
            print(f"partial results: {output}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
