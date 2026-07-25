#!/usr/bin/env python3
"""不启动 QEMU，验证 xv6 文件系统布局分析器和确定性工作负载。"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = REPO_ROOT / "tools" / "fs_locality.py"
SPEC = importlib.util.spec_from_file_location("fs_locality", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load locality module from {MODULE_PATH}")
LOCALITY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = LOCALITY
SPEC.loader.exec_module(LOCALITY)


class PlacementMetricTests(unittest.TestCase):
    """验证连续区间、间隔和 inode 距离的纯计算契约。"""

    def test_contiguous_blocks_form_one_extent(self) -> None:
        """连续数据块只能形成一个 extent，块间最大间隔为一。"""

        metrics = LOCALITY.placement_metrics(10, [20, 21, 22, 23])

        self.assertEqual(1, metrics.data_extent_count)
        self.assertEqual(1, metrics.max_data_gap_blocks)
        self.assertEqual(4, metrics.allocation_span_blocks)
        self.assertEqual(10, metrics.inode_first_data_distance_blocks)

    def test_index_gap_splits_data_extents(self) -> None:
        """索引块插入数据块序列时，不能误报为完整连续数据 extent。"""

        metrics = LOCALITY.placement_metrics(10, [20, 21, 23, 24])

        self.assertEqual(2, metrics.data_extent_count)
        self.assertEqual(2, metrics.max_data_gap_blocks)
        self.assertEqual(5, metrics.allocation_span_blocks)

    def test_empty_file_has_no_distance(self) -> None:
        """空文件没有数据块，也就不存在伪造的元数据距离。"""

        metrics = LOCALITY.placement_metrics(10, [])

        self.assertEqual(0, metrics.data_extent_count)
        self.assertIsNone(metrics.inode_first_data_distance_blocks)


class ImageValidationTests(unittest.TestCase):
    """验证损坏镜像会快速失败而不是生成误导性指标。"""

    def test_invalid_magic_is_rejected(self) -> None:
        """全零镜像不应被误识别为可分析的 xv6 文件系统。"""

        with tempfile.TemporaryDirectory(prefix="xv6-fs-invalid-") as directory:
            image = Path(directory) / "invalid.img"
            image.write_bytes(bytes(2 * LOCALITY.BSIZE))

            with self.assertRaisesRegex(LOCALITY.ExperimentError, "invalid xv6"):
                LOCALITY.FsImage(image)


class WorkloadContractTests(unittest.TestCase):
    """验证负载参数确实跨越目录和一级间接块边界。"""

    def test_small_file_count_must_grow_root_directory(self) -> None:
        """少于 63 个子项无法稳定触发根目录第二个数据块。"""

        with self.assertRaisesRegex(LOCALITY.ExperimentError, "at least 63"):
            LOCALITY.WorkloadSpec(small_files=62).validate()

    def test_large_file_must_cross_direct_block_boundary(self) -> None:
        """大文件不跨越 NDIRECT 时不能形成索引块观察。"""

        with self.assertRaisesRegex(LOCALITY.ExperimentError, "exceed NDIRECT"):
            LOCALITY.WorkloadSpec(large_blocks=LOCALITY.NDIRECT).validate()


class MkfsImageIntegrationTests(unittest.TestCase):
    """使用仓库真实 mkfs 验证单一全局布局的可观察证据。"""

    @classmethod
    def setUpClass(cls) -> None:
        """生成一次共享实验镜像，避免每个断言重复写入完整磁盘文件。"""

        cls._temporary = tempfile.TemporaryDirectory(prefix="xv6-fs-locality-")
        cls.output_dir = Path(cls._temporary.name) / "artifacts"
        cls.spec = LOCALITY.WorkloadSpec()
        mkfs = REPO_ROOT / "mkfs" / "mkfs"
        if not mkfs.is_file():
            # 复用仓库正式构建目标，避免测试复制 mkfs 的编译参数。
            subprocess.run(
                ["make", "mkfs/mkfs"],
                cwd=REPO_ROOT,
                check=True,
            )
        cls.result = LOCALITY.run_experiment(mkfs, cls.output_dir, cls.spec)

    @classmethod
    def tearDownClass(cls) -> None:
        """删除测试镜像、负载文件和报告，验证实验不会污染仓库。"""

        temporary = getattr(cls, "_temporary", None)
        if temporary is not None:
            temporary.cleanup()

    def test_root_directory_growth_is_measured(self) -> None:
        """目录项超过单块容量后，报告必须显示两个相隔的数据块。"""

        root = self.result.find(".")

        self.assertEqual(self.spec.small_files + 3, self.result.root_entry_count)
        self.assertEqual(2, root.metrics.data_block_count)
        self.assertEqual(2, root.metrics.data_extent_count)
        self.assertGreater(root.metrics.max_data_gap_blocks, 1)

    def test_large_file_exposes_indirect_metadata_gap(self) -> None:
        """大文件跨过直接块后，应解析一个一级索引块和两个数据 extent。"""

        large = self.result.find("large")

        self.assertEqual(self.spec.large_bytes, large.size_bytes)
        self.assertEqual(self.spec.large_blocks, large.metrics.data_block_count)
        self.assertEqual(1, len(large.index_blocks))
        self.assertEqual(2, large.metrics.data_extent_count)
        self.assertEqual(2, large.metrics.max_data_gap_blocks)
        self.assertEqual(large.data_blocks[LOCALITY.NDIRECT - 1] + 1, large.index_blocks[0])
        self.assertEqual(large.index_blocks[0] + 1, large.data_blocks[LOCALITY.NDIRECT])

    def test_mkfs_allocates_child_inodes_in_input_order(self) -> None:
        """输入顺序对应连续 inode 编号，证明 mkfs 没有目录块组选择。"""

        child_layouts = [
            layout for layout in self.result.layouts if layout.name not in (".", "..")
        ]

        self.assertEqual(
            list(range(2, 2 + len(child_layouts))),
            [layout.inum for layout in child_layouts],
        )

    def test_report_denies_ffs_and_performance_claims(self) -> None:
        """报告必须明确块组缺失，并拒绝把块位置直接升级为速度结论。"""

        report = self.result.report_text

        self.assertIn("block_groups=absent", report)
        self.assertIn("placement_model=single-global-next-free", report)
        self.assertIn("performance_claim=not-measured", report)
        self.assertIn("FSLOCALITY done status=0", report)

    def test_report_artifacts_are_reproducible(self) -> None:
        """实验必须同时产出文本、JSON、mkfs 日志和可复核镜像。"""

        expected = {
            "fs-locality.img",
            "mkfs.log",
            "report.json",
            "report.txt",
            "workload",
        }

        self.assertEqual(expected, {path.name for path in self.output_dir.iterdir()})
        self.assertEqual(
            self.result.report_text,
            (self.output_dir / "report.txt").read_text(encoding="utf-8"),
        )
        self.assertTrue((self.output_dir / "fs-locality.img").stat().st_size > 0)


if __name__ == "__main__":
    unittest.main()
