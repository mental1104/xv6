"""验证 RAID1 实验的离线块读取和确定性损坏辅助函数。"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from raid1_experiment import BLOCK_SIZE, _corrupt_block, _read_block


class Raid1ExperimentHelpersTest(unittest.TestCase):
    """覆盖宿主机镜像操作的边界，不启动 QEMU。"""

    def test_corrupt_block_only_changes_selected_block(self) -> None:
        """清零目标块时必须保留前后相邻块内容。"""

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "member.img"
            original = bytes((index % 251 for index in range(BLOCK_SIZE * 3)))
            path.write_bytes(original)

            _corrupt_block(path, 1)

            self.assertEqual(_read_block(path, 0), original[:BLOCK_SIZE])
            self.assertEqual(_read_block(path, 1), bytes(BLOCK_SIZE))
            self.assertEqual(_read_block(path, 2), original[BLOCK_SIZE * 2 :])

    def test_read_block_rejects_short_image(self) -> None:
        """镜像不足一个完整块时必须显式失败，不能返回截断证据。"""

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "short.img"
            path.write_bytes(bytes(BLOCK_SIZE - 1))

            with self.assertRaises(RuntimeError):
                _read_block(path, 0)


if __name__ == "__main__":
    unittest.main()
