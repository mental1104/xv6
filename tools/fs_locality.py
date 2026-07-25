#!/usr/bin/env python3
"""生成并分析 xv6 文件系统镜像中的块放置证据。"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence

BSIZE = 1024
FSMAGIC = 0x10203040
DINODE_SIZE = 64
DIRSIZ = 14
NDIRECT = 9
NINDIRECT_LEVELS = 3
NINDIRECT = BSIZE // 4
IPB = BSIZE // DINODE_SIZE
SUPERBLOCK_STRUCT = struct.Struct("<8I")
DINODE_STRUCT = struct.Struct("<4HQ12I")
DIRENT_STRUCT = struct.Struct(f"<H{DIRSIZ}s")


class ExperimentError(RuntimeError):
    """表示镜像格式、工作负载或外部命令不满足实验契约。"""


@dataclass(frozen=True)
class Superblock:
    """保存 xv6 磁盘 superblock 中与布局有关的字段。"""

    magic: int
    size: int
    nblocks: int
    ninodes: int
    nlog: int
    logstart: int
    inodestart: int
    bmapstart: int

    @property
    def data_start(self) -> int:
        """返回首个数据块编号。"""

        return self.size - self.nblocks


@dataclass(frozen=True)
class DiskInode:
    """保存一个磁盘 inode 的文件类型、大小与块索引根。"""

    type: int
    major: int
    minor: int
    nlink: int
    size: int
    addrs: tuple[int, ...]


@dataclass(frozen=True)
class PlacementMetrics:
    """描述一个 inode 的数据块连续性和元数据距离。"""

    data_block_count: int
    data_extent_count: int
    max_data_gap_blocks: int
    allocation_span_blocks: int
    inode_first_data_distance_blocks: int | None


@dataclass(frozen=True)
class FileLayout:
    """描述一个目录项对应 inode 的块布局和派生指标。"""

    name: str
    inum: int
    inode_block: int
    file_type: int
    size_bytes: int
    data_blocks: tuple[int, ...]
    index_blocks: tuple[int, ...]
    metrics: PlacementMetrics


@dataclass(frozen=True)
class WorkloadSpec:
    """定义用于观察小文件、目录增长和大文件索引的确定性负载。"""

    small_files: int = 72
    small_bytes: int = 17
    large_blocks: int = 20
    large_tail_bytes: int = 0

    def validate(self) -> None:
        """校验负载能稳定跨越目录块和一级间接块边界。"""

        if self.small_files < 63:
            raise ExperimentError("small_files must be at least 63 to grow the root directory")
        if self.small_bytes <= 0 or self.small_bytes > BSIZE:
            raise ExperimentError("small_bytes must be in [1, 1024]")
        if self.large_blocks <= NDIRECT:
            raise ExperimentError("large_blocks must exceed NDIRECT to allocate an index block")
        if self.large_tail_bytes < 0 or self.large_tail_bytes >= BSIZE:
            raise ExperimentError("large_tail_bytes must be in [0, 1023]")

    @property
    def large_bytes(self) -> int:
        """返回大文件的精确字节数。"""

        return self.large_blocks * BSIZE + self.large_tail_bytes


@dataclass(frozen=True)
class ExperimentResult:
    """汇总一次实际 mkfs 镜像实验的布局和对象证据。"""

    image: str
    superblock: Superblock
    root_entry_count: int
    layouts: tuple[FileLayout, ...]
    report_text: str

    def find(self, name: str) -> FileLayout:
        """按目录项名称返回布局；不存在时抛出实验错误。"""

        for layout in self.layouts:
            if layout.name == name:
                return layout
        raise ExperimentError(f"missing layout for {name!r}")


class FsImage:
    """只读解析当前 xv6 单分区文件系统镜像。"""

    def __init__(self, path: Path) -> None:
        """打开并验证镜像的 superblock 与文件长度。"""

        self.path = path
        self._data = path.read_bytes()
        if len(self._data) < 2 * BSIZE:
            raise ExperimentError("image is too small to contain an xv6 superblock")
        self.superblock = self._read_superblock()
        expected_size = self.superblock.size * BSIZE
        if len(self._data) != expected_size:
            raise ExperimentError(
                f"image length {len(self._data)} does not match superblock size {expected_size}"
            )

    def _read_superblock(self) -> Superblock:
        """从磁盘块 1 解码 superblock，并检查关键范围。"""

        values = SUPERBLOCK_STRUCT.unpack_from(self._data, BSIZE)
        sb = Superblock(*values)
        if sb.magic != FSMAGIC:
            raise ExperimentError(f"invalid xv6 file-system magic: 0x{sb.magic:08x}")
        if not (2 <= sb.logstart <= sb.inodestart <= sb.bmapstart < sb.data_start < sb.size):
            raise ExperimentError("superblock regions are not monotonically ordered")
        return sb

    def read_block(self, block_number: int) -> bytes:
        """读取一个完整块；越界块号视为镜像损坏。"""

        if block_number < 0 or block_number >= self.superblock.size:
            raise ExperimentError(f"block {block_number} is outside the image")
        start = block_number * BSIZE
        return self._data[start : start + BSIZE]

    def read_inode(self, inum: int) -> DiskInode:
        """按 inode 编号读取固定 64 字节磁盘 inode。"""

        if inum <= 0 or inum >= self.superblock.ninodes:
            raise ExperimentError(f"inode {inum} is outside the inode table")
        block_number = inum // IPB + self.superblock.inodestart
        offset = (inum % IPB) * DINODE_SIZE
        values = DINODE_STRUCT.unpack_from(self.read_block(block_number), offset)
        return DiskInode(
            type=values[0],
            major=values[1],
            minor=values[2],
            nlink=values[3],
            size=values[4],
            addrs=tuple(values[5:]),
        )

    def inode_block(self, inum: int) -> int:
        """返回 inode 所在的磁盘块编号。"""

        return inum // IPB + self.superblock.inodestart

    def resolve_inode_blocks(self, inode: DiskInode) -> tuple[tuple[int, ...], tuple[int, ...]]:
        """按逻辑顺序解析 inode 的数据块，并收集经过的索引块。"""

        needed = (inode.size + BSIZE - 1) // BSIZE
        data_blocks: list[int] = []
        index_blocks: list[int] = []

        for address in inode.addrs[:NDIRECT]:
            if len(data_blocks) >= needed:
                break
            if address == 0:
                raise ExperimentError("sparse data block is not supported by this locality experiment")
            data_blocks.append(address)

        for depth in range(1, NINDIRECT_LEVELS + 1):
            if len(data_blocks) >= needed:
                break
            root = inode.addrs[NDIRECT + depth - 1]
            if root == 0:
                raise ExperimentError("inode size requires an absent indirect root")
            self._walk_indirect(root, depth, needed, data_blocks, index_blocks)

        if len(data_blocks) != needed:
            raise ExperimentError(
                f"resolved {len(data_blocks)} data blocks for inode requiring {needed}"
            )
        return tuple(data_blocks), tuple(index_blocks)

    def _walk_indirect(
        self,
        block_number: int,
        depth: int,
        needed: int,
        data_blocks: list[int],
        index_blocks: list[int],
    ) -> None:
        """深度优先展开一个间接树，直到收集到文件大小需要的块数。"""

        if depth < 1 or depth > NINDIRECT_LEVELS:
            raise ExperimentError(f"unsupported indirect depth {depth}")
        index_blocks.append(block_number)
        entries = struct.unpack(f"<{NINDIRECT}I", self.read_block(block_number))
        for address in entries:
            if len(data_blocks) >= needed:
                return
            if address == 0:
                continue
            if depth == 1:
                data_blocks.append(address)
            else:
                self._walk_indirect(
                    address,
                    depth - 1,
                    needed,
                    data_blocks,
                    index_blocks,
                )

    def read_root_entries(self) -> tuple[tuple[str, int], ...]:
        """读取根目录中所有非空目录项，并保持磁盘顺序。"""

        root = self.read_inode(1)
        data_blocks, _ = self.resolve_inode_blocks(root)
        raw = b"".join(self.read_block(block) for block in data_blocks)[: root.size]
        entries: list[tuple[str, int]] = []
        for offset in range(0, len(raw), DIRENT_STRUCT.size):
            inum, raw_name = DIRENT_STRUCT.unpack_from(raw, offset)
            if inum == 0:
                continue
            name = raw_name.split(b"\0", 1)[0].decode("ascii", errors="strict")
            entries.append((name, inum))
        return tuple(entries)

    def layout_for(self, name: str, inum: int) -> FileLayout:
        """解析一个 inode 并计算连续性与元数据距离指标。"""

        inode = self.read_inode(inum)
        data_blocks, index_blocks = self.resolve_inode_blocks(inode)
        inode_block = self.inode_block(inum)
        return FileLayout(
            name=name,
            inum=inum,
            inode_block=inode_block,
            file_type=inode.type,
            size_bytes=inode.size,
            data_blocks=data_blocks,
            index_blocks=index_blocks,
            metrics=placement_metrics(inode_block, data_blocks),
        )


def placement_metrics(inode_block: int, data_blocks: Sequence[int]) -> PlacementMetrics:
    """从有序数据块列表计算连续区间、最大间隔与 inode 距离。"""

    if not data_blocks:
        return PlacementMetrics(0, 0, 0, 0, None)

    extent_count = 1
    max_gap = 0
    for previous, current in zip(data_blocks, data_blocks[1:]):
        gap = abs(current - previous)
        max_gap = max(max_gap, gap)
        if current != previous + 1:
            extent_count += 1

    span = max(data_blocks) - min(data_blocks) + 1
    return PlacementMetrics(
        data_block_count=len(data_blocks),
        data_extent_count=extent_count,
        max_data_gap_blocks=max_gap,
        allocation_span_blocks=span,
        inode_first_data_distance_blocks=abs(data_blocks[0] - inode_block),
    )


def write_workload(directory: Path, spec: WorkloadSpec) -> tuple[str, ...]:
    """在指定目录生成名称稳定、内容确定的 mkfs 输入文件。"""

    spec.validate()
    directory.mkdir(parents=True, exist_ok=True)
    names: list[str] = []

    for index in range(spec.small_files):
        name = f"s{index:02d}"
        payload = bytes(((index + offset) % 251 for offset in range(spec.small_bytes)))
        (directory / name).write_bytes(payload)
        names.append(name)

    large_name = "large"
    chunk = bytes((offset % 251 for offset in range(BSIZE)))
    with (directory / large_name).open("wb") as stream:
        for _ in range(spec.large_blocks):
            stream.write(chunk)
        stream.write(chunk[: spec.large_tail_bytes])
    names.append(large_name)
    return tuple(names)


def build_image(mkfs: Path, image: Path, workload: Path, names: Sequence[str]) -> str:
    """调用仓库现有 mkfs，保存标准输出并生成实验镜像。"""

    mkfs_path = mkfs.resolve()
    if not mkfs_path.is_file():
        raise ExperimentError(f"mkfs executable does not exist: {mkfs_path}")
    image.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        [str(mkfs_path), str(image.resolve()), *names],
        cwd=workload,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if completed.returncode != 0:
        raise ExperimentError(
            f"mkfs failed with status {completed.returncode}:\n{completed.stdout}"
        )
    return completed.stdout


def analyze_image(image: Path) -> tuple[FsImage, tuple[tuple[str, int], ...], tuple[FileLayout, ...]]:
    """解析根目录及其全部命名对象，返回可复核的布局模型。"""

    fs = FsImage(image)
    entries = fs.read_root_entries()
    layouts = tuple(fs.layout_for(name, inum) for name, inum in entries)
    return fs, entries, layouts


def format_blocks(blocks: Iterable[int]) -> str:
    """把块号序列压缩成便于人工核对的连续区间。"""

    values = list(blocks)
    if not values:
        return "-"
    ranges: list[str] = []
    start = previous = values[0]
    for current in values[1:]:
        if current == previous + 1:
            previous = current
            continue
        ranges.append(str(start) if start == previous else f"{start}-{previous}")
        start = previous = current
    ranges.append(str(start) if start == previous else f"{start}-{previous}")
    return ",".join(ranges)


def format_report(
    image: Path,
    spec: WorkloadSpec,
    fs: FsImage,
    entries: Sequence[tuple[str, int]],
    layouts: Sequence[FileLayout],
) -> str:
    """生成机器可匹配、人工可解释的局部性证据报告。"""

    sb = fs.superblock
    by_name = {layout.name: layout for layout in layouts}
    root = by_name["."]
    large = by_name["large"]
    child_inums = [inum for name, inum in entries if name not in (".", "..")]
    inodes_sequential = child_inums == list(range(2, 2 + len(child_inums)))
    small_layouts = [by_name[f"s{index:02d}"] for index in range(spec.small_files)]
    all_small_files_single_extent = all(
        layout.metrics.data_extent_count == 1 for layout in small_layouts
    )
    first_small = small_layouts[0]
    last_small = small_layouts[-1]

    lines = [
        "FSLOCALITY version=1",
        f"image={image}",
        (
            "regions "
            "boot=[0,1) super=[1,2) "
            f"log=[{sb.logstart},{sb.logstart + sb.nlog}) "
            f"inode=[{sb.inodestart},{sb.bmapstart}) "
            f"bitmap=[{sb.bmapstart},{sb.data_start}) "
            f"data=[{sb.data_start},{sb.size})"
        ),
        (
            "workload "
            f"small_files={spec.small_files} small_bytes={spec.small_bytes} "
            f"large_blocks={spec.large_blocks} large_bytes={spec.large_bytes} "
            f"root_entries={len(entries)}"
        ),
        (
            "metrics "
            "data_extent=contiguous-data-run "
            "max_gap=absolute-distance-between-consecutive-data-blocks"
        ),
        (
            "root "
            f"inum={root.inum} inode_block={root.inode_block} "
            f"data_blocks={format_blocks(root.data_blocks)} "
            f"data_extents={root.metrics.data_extent_count} "
            f"max_gap={root.metrics.max_data_gap_blocks}"
        ),
        (
            "small_files "
            f"first={first_small.name}:inum={first_small.inum}:data={format_blocks(first_small.data_blocks)} "
            f"last={last_small.name}:inum={last_small.inum}:data={format_blocks(last_small.data_blocks)} "
            f"all_single_extent={str(all_small_files_single_extent).lower()}"
        ),
        (
            "large "
            f"inum={large.inum} inode_block={large.inode_block} "
            f"data_blocks={format_blocks(large.data_blocks)} "
            f"index_blocks={format_blocks(large.index_blocks)} "
            f"data_extents={large.metrics.data_extent_count} "
            f"max_gap={large.metrics.max_data_gap_blocks}"
        ),
        (
            "evidence "
            f"child_inodes_sequential={str(inodes_sequential).lower()} "
            "block_groups=absent placement_model=single-global-next-free "
            "performance_claim=not-measured"
        ),
        "FSLOCALITY done status=0",
    ]
    return "\n".join(lines) + "\n"


def result_payload(result: ExperimentResult, spec: WorkloadSpec) -> dict[str, object]:
    """构造可供脚本二次核对的 JSON 数据。"""

    return {
        "version": 1,
        "image": result.image,
        "workload": asdict(spec),
        "superblock": asdict(result.superblock),
        "root_entry_count": result.root_entry_count,
        "layouts": [asdict(layout) for layout in result.layouts],
        "conclusions": {
            "block_groups": "absent",
            "placement_model": "single-global-next-free",
            "performance_claim": "not-measured",
        },
    }


def run_experiment(mkfs: Path, output_dir: Path, spec: WorkloadSpec) -> ExperimentResult:
    """生成工作负载、构建镜像、解析布局并持久化报告。"""

    spec.validate()
    output_dir.mkdir(parents=True, exist_ok=True)
    workload = output_dir / "workload"
    image = output_dir / "fs-locality.img"
    # 只清理本实验拥有的固定产物，避免误删调用者传入的其他目录内容。
    if workload.exists():
        shutil.rmtree(workload)
    for artifact in (
        image,
        output_dir / "mkfs.log",
        output_dir / "report.txt",
        output_dir / "report.json",
    ):
        if artifact.exists():
            artifact.unlink()

    names = write_workload(workload, spec)
    mkfs_log = build_image(mkfs, image, workload, names)
    fs, entries, layouts = analyze_image(image)
    report = format_report(image, spec, fs, entries, layouts)
    result = ExperimentResult(
        image=str(image),
        superblock=fs.superblock,
        root_entry_count=len(entries),
        layouts=layouts,
        report_text=report,
    )
    (output_dir / "mkfs.log").write_text(mkfs_log, encoding="utf-8")
    (output_dir / "report.txt").write_text(report, encoding="utf-8")
    (output_dir / "report.json").write_text(
        json.dumps(result_payload(result, spec), ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return result


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    """解析实验命令行参数。"""

    parser = argparse.ArgumentParser(
        description="生成并分析 xv6 单一全局布局的可核对块位置证据",
    )
    parser.add_argument("--mkfs", type=Path, default=Path("mkfs/mkfs"))
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("artifacts/fs-locality"),
    )
    parser.add_argument("--small-files", type=int, default=72)
    parser.add_argument("--small-bytes", type=int, default=17)
    parser.add_argument("--large-blocks", type=int, default=20)
    parser.add_argument("--large-tail-bytes", type=int, default=0)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    """执行完整实验；成功打印报告，失败返回非零状态。"""

    args = parse_args(sys.argv[1:] if argv is None else argv)
    spec = WorkloadSpec(
        small_files=args.small_files,
        small_bytes=args.small_bytes,
        large_blocks=args.large_blocks,
        large_tail_bytes=args.large_tail_bytes,
    )
    try:
        result = run_experiment(args.mkfs, args.output_dir, spec)
    except (ExperimentError, OSError) as error:
        print(f"FSLOCALITY error: {error}", file=sys.stderr)
        return 1
    print(result.report_text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
