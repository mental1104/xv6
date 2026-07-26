#!/usr/bin/env python3
"""通过多次 QEMU 启动验证 RAID1 镜像、降级读取、离线损坏与读时修复。"""

from __future__ import annotations

import argparse
import shutil
import shlex
from dataclasses import dataclass
from pathlib import Path

import pexpect

REPO_ROOT = Path(__file__).resolve().parents[1]
QEMU_PROMPT = (
    "\x1b[1;38;5;208mroot@xv6\x1b[0m:"
    "\x1b[1;34m/root\x1b[0m# "
)
BLOCK_SIZE = 1024
MEMBER_SIZE = 4 * 1024 * 1024
TEST_PROGRAM = "/usr/lib/xv6/tests/raid1test"


@dataclass(frozen=True)
class GuestCommand:
    """描述一条 guest 命令及其输出中必须出现的稳定事实。"""

    command: str
    expected: tuple[str, ...]


class ExperimentFailure(RuntimeError):
    """表示 QEMU、guest 断言或宿主机镜像断言未满足。"""


def _create_zero_image(path: Path, size: int = MEMBER_SIZE) -> None:
    """创建固定容量的稀疏零镜像，覆盖旧实验产物。"""

    with path.open("wb") as image:
        image.truncate(size)


def _read_block(path: Path, blockno: int) -> bytes:
    """读取成员镜像中的一个 RAID1 物理块。"""

    with path.open("rb") as image:
        image.seek(blockno * BLOCK_SIZE)
        data = image.read(BLOCK_SIZE)
    if len(data) != BLOCK_SIZE:
        raise ExperimentFailure(f"short block read: {path} block={blockno}")
    return data


def _corrupt_block(path: Path, blockno: int) -> None:
    """离线清零一个成员块，模拟可校验但不依赖时序的单盘数据损坏。"""

    with path.open("r+b") as image:
        image.seek(blockno * BLOCK_SIZE)
        image.write(bytes(BLOCK_SIZE))
        image.flush()


def _qemu_command(
    root_image: Path,
    member0: Path | None,
    member1: Path | None,
    cpus: int,
) -> str:
    """构造保持成员槽位稳定的 make qemu 命令。"""

    parts = [
        "make",
        "-s",
        "--no-print-directory",
        "qemu",
        f"CPUS={cpus}",
        f"FSIMG={root_image}",
    ]
    if member0 is not None:
        parts.append(f"RAID1_MEMBER0={member0}")
    if member1 is not None:
        parts.append(f"RAID1_MEMBER1={member1}")
    return " ".join(shlex.quote(part) for part in parts)


def _stop_qemu(child: pexpect.spawn) -> None:
    """通过 QEMU monitor 快捷键退出，并在异常路径强制回收进程。"""

    if not child.isalive():
        return
    child.sendcontrol("a")
    child.send("x")
    try:
        child.expect(pexpect.EOF, timeout=5)
    except (pexpect.EOF, pexpect.TIMEOUT):
        child.terminate(force=True)


def _run_boot(
    label: str,
    root_image: Path,
    member0: Path | None,
    member1: Path | None,
    commands: tuple[GuestCommand, ...],
    cpus: int,
    artifact_dir: Path,
) -> None:
    """启动一轮 QEMU，在同一拓扑内顺序执行命令并保存完整串口日志。"""

    command = _qemu_command(root_image, member0, member1, cpus)
    child = pexpect.spawn(
        "/bin/bash",
        ["-lc", command],
        cwd=str(REPO_ROOT),
        encoding="utf-8",
        codec_errors="replace",
        timeout=120,
    )
    chunks: list[str] = [f"$ {command}\n"]
    try:
        child.expect_exact(QEMU_PROMPT)
        chunks.append(child.before or "")
        for guest in commands:
            child.timeout = 120
            child.sendline(guest.command)
            child.expect_exact(QEMU_PROMPT)
            output = child.before or ""
            chunks.append(f"$ {guest.command}\n{output}")
            for expected in guest.expected:
                if expected not in output:
                    raise ExperimentFailure(
                        f"{label}: missing {expected!r} from {guest.command!r}"
                    )
            for rejected in ("panic:", "kerneltrap", "RAID1TEST error="):
                if rejected in output:
                    raise ExperimentFailure(
                        f"{label}: matched {rejected!r} in {guest.command!r}"
                    )
    except (pexpect.EOF, pexpect.TIMEOUT) as exc:
        chunks.append(child.before or "")
        raise ExperimentFailure(f"{label}: QEMU did not complete: {exc}") from exc
    finally:
        _stop_qemu(child)
        (artifact_dir / f"{label}.log").write_text(
            "\n".join(chunks), encoding="utf-8", errors="replace"
        )


def run_experiment(cpus: int, artifact_dir: Path) -> None:
    """执行双盘写入、单盘启动、离线损坏、自愈和重启复验闭环。"""

    if cpus < 1:
        raise ExperimentFailure("cpus must be at least one")

    artifact_dir.mkdir(parents=True, exist_ok=True)
    for child in artifact_dir.iterdir():
        if child.is_file() or child.is_symlink():
            child.unlink()
        else:
            shutil.rmtree(child)

    source_root = REPO_ROOT / "fs.img"
    if not source_root.exists():
        raise ExperimentFailure("fs.img is missing; build xv6 before the experiment")
    root_image = artifact_dir / "root.img"
    member0 = artifact_dir / "member0.img"
    member1 = artifact_dir / "member1.img"
    shutil.copyfile(source_root, root_image)
    _create_zero_image(member0)
    _create_zero_image(member1)

    blockno = 7
    seed = 91

    _run_boot(
        "01-mirror-write",
        root_image,
        member0,
        member1,
        (
            GuestCommand(f"{TEST_PROGRAM} info 3", ("present=3",)),
            GuestCommand(
                f"{TEST_PROGRAM} write {blockno} {seed} 3",
                ("op=write", "completed=3", "result=ok"),
            ),
            GuestCommand(
                f"{TEST_PROGRAM} read {blockno} {seed} 3 3 0 0",
                ("valid=3", "source=0", "repaired=0", "result=ok"),
            ),
        ),
        cpus,
        artifact_dir,
    )

    if _read_block(member0, blockno) != _read_block(member1, blockno):
        raise ExperimentFailure("initial mirror blocks differ")

    _run_boot(
        "02-member0-missing",
        root_image,
        None,
        member1,
        (
            GuestCommand(f"{TEST_PROGRAM} info 2", ("present=2",)),
            GuestCommand(
                f"{TEST_PROGRAM} read {blockno} {seed} 2 2 1 0",
                ("valid=2", "source=1", "repaired=0", "result=ok"),
            ),
        ),
        cpus,
        artifact_dir,
    )

    _corrupt_block(member0, blockno)
    if _read_block(member0, blockno) == _read_block(member1, blockno):
        raise ExperimentFailure("fault injection did not diverge mirror blocks")

    _run_boot(
        "03-corruption-repair",
        root_image,
        member0,
        member1,
        (
            GuestCommand(
                f"{TEST_PROGRAM} read {blockno} {seed} 3 2 1 1",
                ("valid=2", "source=1", "repaired=1", "result=ok"),
            ),
        ),
        cpus,
        artifact_dir,
    )

    if _read_block(member0, blockno) != _read_block(member1, blockno):
        raise ExperimentFailure("read repair did not restore identical blocks")

    _run_boot(
        "04-restart-clean",
        root_image,
        member0,
        member1,
        (
            GuestCommand(
                f"{TEST_PROGRAM} read {blockno} {seed} 3 3 0 0",
                ("valid=3", "source=0", "repaired=0", "result=ok"),
            ),
        ),
        cpus,
        artifact_dir,
    )

    print(
        "RAID1 EXPERIMENT PASS "
        f"cpus={cpus} block={blockno} artifacts={artifact_dir.relative_to(REPO_ROOT)}"
    )


def parse_args() -> argparse.Namespace:
    """解析 CPU 数量和实验产物目录。"""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, default=1)
    parser.add_argument(
        "--artifacts",
        type=Path,
        default=REPO_ROOT / "artifacts" / "raid1",
    )
    return parser.parse_args()


def main() -> int:
    """运行实验，并将失败统一转换为非零退出状态和稳定错误文本。"""

    args = parse_args()
    try:
        run_experiment(args.cpus, args.artifacts.resolve())
    except ExperimentFailure as exc:
        print(f"RAID1 EXPERIMENT ERROR: {exc}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
