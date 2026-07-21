#!/usr/bin/env python3
"""Build and run all four xv6 vmbench variants, then render reports."""

from __future__ import annotations

import argparse
import json
import os
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import BinaryIO, Callable

VARIANTS = {
    "pristine": "concept/21-vmbench-pristine",
    "lazy": "concept/21-vmbench-lazy",
    "cow": "concept/21-vmbench-cow",
    "main": "concept/21-vmbench-main",
}
EXPECTED_RESULTS = 9
PROMPT = b"$ "
MAX_CAPTURE_BYTES = 2_000_000
DIAGNOSTIC_TAIL_BYTES = 16_384


@dataclass
class RunRecord:
    variant: str
    branch: str
    commit: str
    worktree: str
    build_log: str
    qemu_log: str
    qemu_host_log: str
    build_ok: bool = False
    qemu_ok: bool = False


def run_checked(
    command: list[str],
    *,
    cwd: Path,
    log_path: Path | None = None,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("w", encoding="utf-8") as stream:
            return subprocess.run(
                command,
                cwd=cwd,
                text=True,
                stdout=stream,
                stderr=subprocess.STDOUT,
                check=True,
            )
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        capture_output=capture,
        check=True,
    )


def git_output(repo: Path, *args: str) -> str:
    result = run_checked(["git", *args], cwd=repo, capture=True)
    return result.stdout.strip()


def ensure_prerequisites(repo: Path, *, require_runtime: bool) -> None:
    commands = ["git", "make", "python3"]
    if require_runtime:
        commands.append("qemu-system-riscv64")
    missing = [name for name in commands if shutil.which(name) is None]
    if missing:
        raise RuntimeError("missing required commands: " + ", ".join(missing))
    git_output(repo, "rev-parse", "--git-dir")


def output_tail(buffer: bytearray) -> str:
    if not buffer:
        return "<no xv6 serial output captured>"
    return bytes(buffer[-DIAGNOSTIC_TAIL_BYTES:]).decode(
        "utf-8", errors="replace"
    )


def wait_for_socket(
    socket_path: Path,
    process: subprocess.Popen[bytes],
    *,
    timeout: int,
) -> socket.socket:
    deadline = time.monotonic() + min(timeout, 30)
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        exit_code = process.poll()
        if exit_code is not None:
            raise RuntimeError(
                f"QEMU exited before serial socket became available; exit={exit_code}"
            )
        if socket_path.exists():
            client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                client.connect(str(socket_path))
                client.setblocking(False)
                return client
            except OSError as error:
                last_error = error
                client.close()
        time.sleep(0.05)
    detail = f": {last_error}" if last_error else ""
    raise TimeoutError(
        f"timed out waiting for QEMU serial socket {socket_path}{detail}"
    )


def wait_for(
    serial: socket.socket,
    process: subprocess.Popen[bytes],
    log_stream: BinaryIO,
    predicate: Callable[[bytes], bool],
    *,
    timeout: int,
    description: str,
) -> bytes:
    deadline = time.monotonic() + timeout
    buffer = bytearray()

    while time.monotonic() < deadline:
        exit_code = process.poll()
        if exit_code is not None:
            raise RuntimeError(
                f"QEMU exited before {description}; exit={exit_code}\n"
                f"--- xv6 serial output tail ---\n{output_tail(buffer)}"
            )

        ready, _, _ = select.select([serial], [], [], 0.5)
        if not ready:
            continue

        try:
            chunk = serial.recv(65536)
        except BlockingIOError:
            continue
        except OSError as error:
            raise RuntimeError(
                f"failed reading xv6 serial output while waiting for "
                f"{description}: {error}\n"
                f"--- xv6 serial output tail ---\n{output_tail(buffer)}"
            ) from error

        if not chunk:
            continue

        log_stream.write(chunk)
        log_stream.flush()
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()

        buffer.extend(chunk)
        if len(buffer) > MAX_CAPTURE_BYTES:
            del buffer[: len(buffer) - MAX_CAPTURE_BYTES // 2]

        if predicate(bytes(buffer)):
            return bytes(buffer)

    raise TimeoutError(
        f"timed out after {timeout}s waiting for {description}\n"
        f"--- xv6 serial output tail ---\n{output_tail(buffer)}"
    )


def shell_prompt_seen(data: bytes) -> bool:
    stripped = data.rstrip()
    return PROMPT in data or stripped.endswith(b"$")


def terminate_qemu(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            return
        process.wait(timeout=5)


def qemu_command(worktree: Path, serial_socket: Path) -> list[str]:
    qemu = shutil.which("qemu-system-riscv64")
    if qemu is None:
        raise RuntimeError("qemu-system-riscv64 not found")
    return [
        qemu,
        "-machine",
        "virt",
        "-bios",
        "none",
        "-kernel",
        str(worktree / "kernel" / "kernel"),
        "-m",
        "128M",
        "-smp",
        "1",
        "-display",
        "none",
        "-monitor",
        "none",
        "-chardev",
        (
            "socket,id=xv6serial,"
            f"path={serial_socket},server=on,wait=on"
        ),
        "-serial",
        "chardev:xv6serial",
        "-drive",
        (
            f"file={worktree / 'fs.img'},if=none,"
            "format=raw,id=x0"
        ),
        "-device",
        "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
    ]


def run_qemu(
    worktree: Path,
    log_path: Path,
    host_log_path: Path,
    *,
    pages: int,
    timeout: int,
    regression_commands: list[str],
) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    host_log_path.parent.mkdir(parents=True, exist_ok=True)

    socket_dir = Path(tempfile.mkdtemp(prefix="xv6vm-", dir="/tmp"))
    serial_socket_path = socket_dir / "serial.sock"
    command = qemu_command(worktree, serial_socket_path)

    serial: socket.socket | None = None
    try:
        with host_log_path.open("wb") as host_log:
            process = subprocess.Popen(
                command,
                cwd=worktree,
                stdin=subprocess.DEVNULL,
                stdout=host_log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                serial = wait_for_socket(
                    serial_socket_path,
                    process,
                    timeout=timeout,
                )
                with log_path.open("wb") as log_stream:
                    wait_for(
                        serial,
                        process,
                        log_stream,
                        shell_prompt_seen,
                        timeout=timeout,
                        description="xv6 shell prompt",
                    )

                    serial.sendall(f"vmbench all {pages}\n".encode())
                    output = wait_for(
                        serial,
                        process,
                        log_stream,
                        lambda data: (
                            data.count(b"VMRESULT ") >= EXPECTED_RESULTS
                            and shell_prompt_seen(data)
                        ),
                        timeout=timeout,
                        description=f"{EXPECTED_RESULTS} VMRESULT lines",
                    )
                    if b"VMFAILED " in output or b"VMERROR " in output:
                        raise RuntimeError(
                            "vmbench reported VMFAILED/VMERROR; "
                            "inspect the xv6 serial log"
                        )

                    for regression in regression_commands:
                        serial.sendall((regression + "\n").encode())
                        regression_output = wait_for(
                            serial,
                            process,
                            log_stream,
                            shell_prompt_seen,
                            timeout=timeout,
                            description=f"completion of {regression}",
                        )
                        success_marker = (
                            b"ALL COW TESTS PASSED"
                            if regression == "cowtest"
                            else b"ALL TESTS PASSED"
                        )
                        if success_marker not in regression_output:
                            raise RuntimeError(
                                f"{regression} did not print "
                                f"{success_marker.decode()!r}; "
                                "inspect the xv6 serial log"
                            )
            finally:
                if serial is not None:
                    serial.close()
                terminate_qemu(process)
    finally:
        shutil.rmtree(socket_dir, ignore_errors=True)


def add_worktree(repo: Path, directory: Path, ref: str) -> None:
    if directory.exists():
        run_checked(
            ["git", "worktree", "remove", "--force", str(directory)],
            cwd=repo,
        )
    run_checked(
        ["git", "worktree", "add", "--detach", str(directory), ref],
        cwd=repo,
    )


def remove_worktree(repo: Path, directory: Path) -> None:
    if directory.exists():
        subprocess.run(
            ["git", "worktree", "remove", "--force", str(directory)],
            cwd=repo,
            text=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )


def regression_for(variant: str, enabled: bool) -> list[str]:
    if not enabled:
        return []
    commands: list[str] = []
    if variant in ("lazy", "main"):
        commands.append("lazytests")
    if variant in ("cow", "main"):
        commands.append("cowtest")
    commands.append("usertests")
    return commands


def write_manifest(
    output_dir: Path,
    *,
    pages: int,
    with_regression: bool,
    records: list[RunRecord],
    error: str | None,
) -> None:
    manifest = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "pages": pages,
        "cpus": 1,
        "with_regression": with_regression,
        "error": error,
        "records": [record.__dict__ for record in records],
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Fetch, build, run and compare the four xv6 vmbench branches."
        )
    )
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path.cwd(),
        help="xv6 repository root",
    )
    parser.add_argument("--remote", default="origin")
    parser.add_argument("--pages", type=int, default=4096)
    parser.add_argument(
        "--timeout",
        type=int,
        default=300,
        help="timeout for each QEMU phase",
    )
    parser.add_argument("--output", type=Path, help="artifact directory")
    parser.add_argument(
        "--with-regression",
        action="store_true",
        help="also run lazytests/cowtest/usertests",
    )
    parser.add_argument("--keep-worktrees", action="store_true")
    parser.add_argument("--skip-fetch", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.pages <= 0:
        parser.error("--pages must be positive")

    repo = args.repo.resolve()
    records: list[RunRecord] = []
    output_dir: Path | None = None

    try:
        ensure_prerequisites(repo, require_runtime=not args.dry_run)
        repo_root = Path(
            git_output(repo, "rev-parse", "--show-toplevel")
        ).resolve()
        timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        output_dir = (
            args.output
            or (
                repo_root.parent
                / f"{repo_root.name}-vmbench-results"
                / timestamp
            )
        ).resolve()
        logs_dir = output_dir / "logs"
        build_dir = output_dir / "build"
        charts_dir = output_dir / "charts"
        worktree_root = Path(
            tempfile.mkdtemp(prefix=f"{repo_root.name}-vmbench-worktrees-")
        )

        refs = {
            variant: f"{args.remote}/{branch}"
            for variant, branch in VARIANTS.items()
        }
        print("VM benchmark plan:")
        for variant, ref in refs.items():
            print(f"  {variant:8} <- {ref}")
        print(f"  pages     = {args.pages}")
        print(f"  output    = {output_dir}")
        print(f"  regression= {args.with_regression}")

        if args.dry_run:
            shutil.rmtree(worktree_root, ignore_errors=True)
            return 0

        output_dir.mkdir(parents=True, exist_ok=True)
        if not args.skip_fetch:
            run_checked(
                ["git", "fetch", "--prune", args.remote],
                cwd=repo_root,
            )

        created_worktrees: list[Path] = []
        run_error: str | None = None
        try:
            for variant, branch in VARIANTS.items():
                ref = refs[variant]
                worktree = worktree_root / variant

                print(f"\n==> [{variant}] checkout {ref}")
                add_worktree(repo_root, worktree, ref)
                created_worktrees.append(worktree)

                commit = git_output(worktree, "rev-parse", "HEAD")
                record = RunRecord(
                    variant=variant,
                    branch=branch,
                    commit=commit,
                    worktree=str(worktree),
                    build_log=str(build_dir / f"{variant}.log"),
                    qemu_log=str(logs_dir / f"{variant}.log"),
                    qemu_host_log=str(
                        logs_dir / f"{variant}.host.log"
                    ),
                )
                records.append(record)

                print(f"==> [{variant}] make clean && make")
                build_log = Path(record.build_log)
                build_log.parent.mkdir(parents=True, exist_ok=True)
                with build_log.open("w", encoding="utf-8") as stream:
                    subprocess.run(
                        ["make", "clean"],
                        cwd=worktree,
                        text=True,
                        stdout=stream,
                        stderr=subprocess.STDOUT,
                        check=True,
                    )
                    subprocess.run(
                        ["make"],
                        cwd=worktree,
                        text=True,
                        stdout=stream,
                        stderr=subprocess.STDOUT,
                        check=True,
                    )
                record.build_ok = True

                print(
                    f"==> [{variant}] QEMU socket: "
                    f"vmbench all {args.pages}"
                )
                run_qemu(
                    worktree,
                    Path(record.qemu_log),
                    Path(record.qemu_host_log),
                    pages=args.pages,
                    timeout=args.timeout,
                    regression_commands=regression_for(
                        variant,
                        args.with_regression,
                    ),
                )
                record.qemu_ok = True

            main_worktree = worktree_root / "main"
            compare_script = (
                main_worktree / "tools" / "vmbench_compare.py"
            )
            compare_command = [
                sys.executable,
                str(compare_script),
                *(
                    f"{variant}={logs_dir / (variant + '.log')}"
                    for variant in VARIANTS
                ),
                "--csv",
                str(output_dir / "vmbench-results.csv"),
                "--markdown",
                str(output_dir / "vmbench-results.md"),
                "--svg-dir",
                str(charts_dir),
            ]

            print("\n==> compare logs and render SVG charts")
            with (output_dir / "compare.stdout.md").open(
                "w",
                encoding="utf-8",
            ) as stream:
                subprocess.run(
                    compare_command,
                    cwd=main_worktree,
                    text=True,
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                    check=True,
                )
        except Exception as error:
            run_error = str(error)
            raise
        finally:
            write_manifest(
                output_dir,
                pages=args.pages,
                with_regression=args.with_regression,
                records=records,
                error=run_error,
            )
            if not args.keep_worktrees:
                for worktree in reversed(created_worktrees):
                    remove_worktree(repo_root, worktree)
                subprocess.run(
                    ["git", "worktree", "prune"],
                    cwd=repo_root,
                    check=False,
                )
                shutil.rmtree(worktree_root, ignore_errors=True)
            else:
                print(f"Kept worktrees: {worktree_root}")

        print("\nDone.")
        print(f"Results:  {output_dir}")
        print(f"Markdown: {output_dir / 'vmbench-results.md'}")
        print(f"CSV:      {output_dir / 'vmbench-results.csv'}")
        print(f"SVG:      {charts_dir}")
        return 0

    except (
        OSError,
        RuntimeError,
        TimeoutError,
        subprocess.CalledProcessError,
    ) as error:
        print(f"vmbench_run_all: {error}", file=sys.stderr)
        if output_dir is not None:
            print(f"partial results: {output_dir}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
