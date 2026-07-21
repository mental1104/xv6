#!/usr/bin/env python3
"""Build and run all four xv6 vmbench variants, then render reports."""

from __future__ import annotations

import argparse
import json
import os
import pty
import select
import signal
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

VARIANTS = {
    "pristine": "concept/21-vmbench-pristine",
    "lazy": "concept/21-vmbench-lazy",
    "cow": "concept/21-vmbench-cow",
    "main": "concept/21-vmbench-main",
}
EXPECTED_RESULTS = 9
PROMPT = b"$ "


@dataclass
class RunRecord:
    variant: str
    branch: str
    commit: str
    worktree: str
    build_log: str
    qemu_log: str
    build_ok: bool = False
    qemu_ok: bool = False


def run_checked(
    command: list[str],
    *,
    cwd: Path,
    log_path: Path | None = None,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    if log_path:
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
    commands = ["git", "make"]
    if require_runtime:
        commands.append("qemu-system-riscv64")
    missing = [name for name in commands if shutil.which(name) is None]
    if missing:
        raise RuntimeError("missing required commands: " + ", ".join(missing))
    git_output(repo, "rev-parse", "--git-dir")


def wait_for(
    master_fd: int,
    process: subprocess.Popen[bytes],
    log_stream,
    predicate,
    *,
    timeout: int,
    description: str,
) -> bytes:
    deadline = time.monotonic() + timeout
    buffer = bytearray()
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"QEMU exited before {description}; exit={process.returncode}"
            )
        ready, _, _ = select.select([master_fd], [], [], 0.5)
        if not ready:
            continue
        try:
            chunk = os.read(master_fd, 65536)
        except OSError as error:
            raise RuntimeError(
                f"failed reading QEMU output while waiting for {description}: {error}"
            ) from error
        if not chunk:
            continue
        log_stream.buffer.write(chunk)
        log_stream.flush()
        buffer.extend(chunk)
        if len(buffer) > 2_000_000:
            del buffer[: len(buffer) - 1_000_000]
        if predicate(bytes(buffer)):
            return bytes(buffer)
    raise TimeoutError(f"timed out after {timeout}s waiting for {description}")


def terminate_qemu(master_fd: int, process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.write(master_fd, b"\x01x")
        process.wait(timeout=8)
        return
    except Exception:
        pass
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)


def run_qemu(
    worktree: Path,
    log_path: Path,
    *,
    pages: int,
    timeout: int,
    regression_commands: list[str],
) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    master_fd, slave_fd = pty.openpty()
    process = subprocess.Popen(
        ["make", "qemu", "CPUS=1"],
        cwd=worktree,
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        close_fds=True,
        start_new_session=True,
    )
    os.close(slave_fd)

    try:
        with log_path.open("w", encoding="utf-8", errors="replace") as log_stream:
            wait_for(
                master_fd,
                process,
                log_stream,
                lambda data: PROMPT in data,
                timeout=timeout,
                description="xv6 shell prompt",
            )

            os.write(master_fd, f"vmbench all {pages}\n".encode())
            output = wait_for(
                master_fd,
                process,
                log_stream,
                lambda data: data.count(b"VMRESULT ") >= EXPECTED_RESULTS
                and data.rstrip().endswith(b"$"),
                timeout=timeout,
                description=f"{EXPECTED_RESULTS} VMRESULT lines",
            )
            if b"VMFAILED " in output or b"VMERROR " in output:
                raise RuntimeError(
                    "vmbench reported VMFAILED/VMERROR; inspect the QEMU log"
                )

            for regression in regression_commands:
                os.write(master_fd, (regression + "\n").encode())
                regression_output = wait_for(
                    master_fd,
                    process,
                    log_stream,
                    lambda data: data.rstrip().endswith(b"$"),
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
                        f"{regression} did not print {success_marker.decode()!r}; "
                        "inspect the QEMU log"
                    )
    finally:
        terminate_qemu(master_fd, process)
        os.close(master_fd)


def add_worktree(repo: Path, directory: Path, ref: str) -> None:
    if directory.exists():
        run_checked(
            ["git", "worktree", "remove", "--force", str(directory)], cwd=repo
        )
    run_checked(
        ["git", "worktree", "add", "--detach", str(directory), ref], cwd=repo
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


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Fetch, build, run and compare the four xv6 vmbench experiment branches."
        )
    )
    parser.add_argument(
        "--repo", type=Path, default=Path.cwd(), help="xv6 repository root"
    )
    parser.add_argument("--remote", default="origin")
    parser.add_argument("--pages", type=int, default=4096)
    parser.add_argument(
        "--timeout", type=int, default=300, help="timeout for each QEMU phase"
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

    repo = args.repo.resolve()
    if args.pages <= 0:
        parser.error("--pages must be positive")

    try:
        ensure_prerequisites(repo, require_runtime=not args.dry_run)
        repo_root = Path(git_output(repo, "rev-parse", "--show-toplevel")).resolve()
        timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        output_dir = (
            args.output
            or (repo_root.parent / f"{repo_root.name}-vmbench-results" / timestamp)
        ).resolve()
        logs_dir = output_dir / "logs"
        build_dir = output_dir / "build"
        charts_dir = output_dir / "charts"
        worktree_root = Path(
            tempfile.mkdtemp(prefix=f"{repo_root.name}-vmbench-worktrees-")
        )

        refs = {
            variant: f"{args.remote}/{branch}" for variant, branch in VARIANTS.items()
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
            run_checked(["git", "fetch", "--prune", args.remote], cwd=repo_root)

        records: list[RunRecord] = []
        created_worktrees: list[Path] = []
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

                print(f"==> [{variant}] QEMU: vmbench all {args.pages}")
                run_qemu(
                    worktree,
                    Path(record.qemu_log),
                    pages=args.pages,
                    timeout=args.timeout,
                    regression_commands=regression_for(
                        variant, args.with_regression
                    ),
                )
                record.qemu_ok = True

            main_worktree = worktree_root / "main"
            compare_script = main_worktree / "tools" / "vmbench_compare.py"
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
                "w", encoding="utf-8"
            ) as stream:
                subprocess.run(
                    compare_command,
                    cwd=main_worktree,
                    text=True,
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                    check=True,
                )

            manifest = {
                "generated_at": datetime.now(timezone.utc).isoformat(),
                "pages": args.pages,
                "cpus": 1,
                "with_regression": args.with_regression,
                "records": [record.__dict__ for record in records],
            }
            (output_dir / "manifest.json").write_text(
                json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
        finally:
            if not args.keep_worktrees:
                for worktree in reversed(created_worktrees):
                    remove_worktree(repo_root, worktree)
                subprocess.run(
                    ["git", "worktree", "prune"], cwd=repo_root, check=False
                )
                shutil.rmtree(worktree_root, ignore_errors=True)
            else:
                print(f"Kept worktrees: {worktree_root}")

        print("\nDone.")
        print(f"Results: {output_dir}")
        print(f"Markdown: {output_dir / 'vmbench-results.md'}")
        print(f"CSV:      {output_dir / 'vmbench-results.csv'}")
        print(f"SVG:      {charts_dir}")
        return 0
    except (OSError, RuntimeError, TimeoutError, subprocess.CalledProcessError) as error:
        print(f"vmbench_run_all: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
