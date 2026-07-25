#!/usr/bin/env python3
"""不启动 QEMU，验证调度入门 trace 报告器的协议与策略判据。"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "sched_intro_report.py"
SPEC = importlib.util.spec_from_file_location("sched_intro_report", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load report module from {MODULE_PATH}")
REPORT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = REPORT
SPEC.loader.exec_module(REPORT)


def event_line(
    seq: int,
    tick: int,
    pid: int,
    event_type: str,
    reason: str,
    runtime: int,
    dispatches: int,
    remaining: int,
    cpu: int = 0,
) -> str:
    """生成最小但完整的 SCHEDTRACE event 测试行。"""

    return (
        f"SCHEDTRACE event seq={seq} ts={seq * 10} tick={tick} cpu={cpu} "
        f"pid={pid} type={event_type} reason={reason} state=4 slice=0 "
        f"runtime={runtime} dispatches={dispatches} remaining={remaining} "
        "level=0 epoch=1 weight=1024 vruntime=0 name=schedviz"
    )


def trace_text(policy: str, events: list[str], cpus: int = 1, dropped: int = 0) -> str:
    """封装带版本头和成功结束标记的 trace。"""

    return "\n".join(
        [
            f"SCHEDTRACE version=1 policy={policy} cpus={cpus} "
            f"events={len(events)} dropped={dropped}",
            *events,
            "SCHEDTRACE done status=0",
            "",
        ]
    )


def legend_text(rows: list[tuple[str, int, int, int]]) -> str:
    """生成 schedviz legend 测试文本。"""

    return "\n".join(
        f"  {glyph} = worker{worker} pid={pid} hint={hint} weight=1024"
        for glyph, worker, pid, hint in rows
    )


class TraceParsingTests(unittest.TestCase):
    """验证 SCHEDTRACE 协议和 legend 的严格解析。"""

    def test_parse_complete_trace_and_legend(self) -> None:
        text = trace_text(
            "fifo",
            [
                event_line(1, 0, 10, "RUN_START", "NONE", 0, 1, 4),
                event_line(2, 4, 10, "RUN_STOP", "EXIT", 4, 1, 0),
            ],
        )
        trace = REPORT.parse_trace_text(text)
        legends = REPORT.parse_legend(legend_text([("A", 0, 10, 4)]))

        self.assertEqual("fifo", trace.policy)
        self.assertEqual(2, len(trace.events))
        self.assertEqual(0, legends[10].worker)
        self.assertEqual(4, legends[10].hint)

    def test_dropped_events_fail_closed(self) -> None:
        text = trace_text(
            "rr",
            [event_line(1, 0, 10, "RUN_START", "NONE", 0, 1, 4)],
            dropped=1,
        )
        with self.assertRaisesRegex(REPORT.ExperimentError, "dropped 1"):
            REPORT.parse_trace_text(text)

    def test_non_increasing_sequence_is_rejected(self) -> None:
        text = trace_text(
            "fifo",
            [
                event_line(2, 0, 10, "RUN_START", "NONE", 0, 1, 4),
                event_line(2, 4, 10, "RUN_STOP", "EXIT", 4, 1, 0),
            ],
        )
        with self.assertRaisesRegex(REPORT.ExperimentError, "strictly increasing"):
            REPORT.parse_trace_text(text)


class PolicyEvidenceTests(unittest.TestCase):
    """验证四类教材策略各自的完成顺序或抢占证据。"""

    def _derive(
        self,
        policy: str,
        events: list[str],
        rows: list[tuple[str, int, int, int]],
    ) -> tuple[object, tuple[object, ...]]:
        trace = REPORT.parse_trace_text(trace_text(policy, events))
        metrics = REPORT.derive_metrics(trace, REPORT.parse_legend(legend_text(rows)))
        return trace, metrics

    def test_fifo_follows_enqueue_order(self) -> None:
        trace, metrics = self._derive(
            "fifo",
            [
                event_line(1, 0, 10, "RUN_START", "NONE", 0, 1, 8),
                event_line(2, 8, 10, "RUN_STOP", "EXIT", 8, 1, 0),
                event_line(3, 8, 11, "RUN_START", "NONE", 0, 1, 2),
                event_line(4, 10, 11, "RUN_STOP", "EXIT", 2, 1, 0),
                event_line(5, 10, 12, "RUN_START", "NONE", 0, 1, 5),
                event_line(6, 15, 12, "RUN_STOP", "EXIT", 5, 1, 0),
            ],
            [("A", 0, 10, 8), ("B", 1, 11, 2), ("C", 2, 12, 5)],
        )

        evidence = REPORT.validate_policy_evidence(trace, metrics, True)

        self.assertIn("enqueue order", evidence)
        self.assertEqual([8, 10, 15], [metric.completion_tick for metric in metrics])

    def test_sjf_follows_burst_hint(self) -> None:
        trace, metrics = self._derive(
            "sjf",
            [
                event_line(1, 0, 11, "RUN_START", "NONE", 0, 1, 2),
                event_line(2, 2, 11, "RUN_STOP", "EXIT", 2, 1, 0),
                event_line(3, 2, 12, "RUN_START", "NONE", 0, 1, 5),
                event_line(4, 7, 12, "RUN_STOP", "EXIT", 5, 1, 0),
                event_line(5, 7, 10, "RUN_START", "NONE", 0, 1, 8),
                event_line(6, 15, 10, "RUN_STOP", "EXIT", 8, 1, 0),
            ],
            [("A", 0, 10, 8), ("B", 1, 11, 2), ("C", 2, 12, 5)],
        )

        evidence = REPORT.validate_policy_evidence(trace, metrics, True)

        self.assertIn("burst hint", evidence)
        by_worker = {metric.worker: metric for metric in metrics}
        self.assertEqual(7, by_worker[0].response_ticks)
        self.assertEqual(0, by_worker[1].response_ticks)

    def test_stcf_uses_preemption_tick_as_short_arrival(self) -> None:
        trace, metrics = self._derive(
            "stcf",
            [
                event_line(1, 0, 20, "RUN_START", "NONE", 0, 1, 12),
                event_line(2, 2, 20, "RUN_STOP", "STCF_SHORTER_TASK", 2, 1, 10),
                event_line(3, 2, 21, "RUN_START", "NONE", 0, 1, 2),
                event_line(4, 4, 21, "RUN_STOP", "EXIT", 2, 1, 0),
                event_line(5, 4, 20, "RUN_START", "NONE", 2, 2, 10),
                event_line(6, 14, 20, "RUN_STOP", "EXIT", 12, 2, 0),
            ],
            [("A", 0, 20, 12), ("B", 1, 21, 2)],
        )

        evidence = REPORT.validate_policy_evidence(trace, metrics, True)
        by_worker = {metric.worker: metric for metric in metrics}

        self.assertIn("shorter-task preemption", evidence)
        self.assertEqual(2, by_worker[1].arrival_tick)
        self.assertEqual(0, by_worker[1].response_ticks)
        self.assertLess(by_worker[1].completion_tick, by_worker[0].completion_tick)

    def test_rr_requires_quantum_rotation_and_repeat_dispatch(self) -> None:
        trace, metrics = self._derive(
            "rr",
            [
                event_line(1, 0, 30, "RUN_START", "NONE", 0, 1, 4),
                event_line(2, 2, 30, "RUN_STOP", "RR_QUANTUM", 2, 1, 2),
                event_line(3, 2, 31, "RUN_START", "NONE", 0, 1, 4),
                event_line(4, 4, 31, "RUN_STOP", "RR_QUANTUM", 2, 1, 2),
                event_line(5, 4, 30, "RUN_START", "NONE", 2, 2, 2),
                event_line(6, 6, 30, "RUN_STOP", "EXIT", 4, 2, 0),
                event_line(7, 6, 31, "RUN_START", "NONE", 2, 2, 2),
                event_line(8, 8, 31, "RUN_STOP", "EXIT", 4, 2, 0),
            ],
            [("A", 0, 30, 4), ("B", 1, 31, 4)],
        )

        evidence = REPORT.validate_policy_evidence(trace, metrics, True)

        self.assertIn("RR quantum", evidence)
        self.assertTrue(all(metric.dispatches == 2 for metric in metrics))

    def test_stcf_smp_metrics_do_not_require_preemption_boundary(self) -> None:
        trace = REPORT.parse_trace_text(
            trace_text(
                "stcf",
                [
                    event_line(1, 0, 20, "RUN_START", "NONE", 0, 1, 12, cpu=0),
                    event_line(2, 0, 21, "RUN_START", "NONE", 0, 1, 2, cpu=1),
                    event_line(3, 2, 21, "RUN_STOP", "EXIT", 2, 1, 0, cpu=1),
                    event_line(4, 12, 20, "RUN_STOP", "EXIT", 12, 1, 0, cpu=0),
                ],
                cpus=3,
            )
        )
        metrics = REPORT.derive_metrics(
            trace,
            REPORT.parse_legend(
                legend_text([("A", 0, 20, 12), ("B", 1, 21, 2)])
            ),
            strict_single_cpu=False,
        )

        evidence = REPORT.validate_policy_evidence(trace, metrics, False)

        self.assertIn("SMP smoke", evidence)
        self.assertTrue(all(metric.arrival_tick == 0 for metric in metrics))

    def test_smp_mode_does_not_apply_single_cpu_order_oracle(self) -> None:
        trace, metrics = self._derive(
            "fifo",
            [
                event_line(1, 0, 10, "RUN_START", "NONE", 0, 1, 8, cpu=0),
                event_line(2, 0, 11, "RUN_START", "NONE", 0, 1, 2, cpu=1),
                event_line(3, 2, 11, "RUN_STOP", "EXIT", 2, 1, 0, cpu=1),
                event_line(4, 8, 10, "RUN_STOP", "EXIT", 8, 1, 0, cpu=0),
            ],
            [("A", 0, 10, 8), ("B", 1, 11, 2)],
        )

        evidence = REPORT.validate_policy_evidence(trace, metrics, False)

        self.assertIn("SMP smoke", evidence)


class CommandContractTests(unittest.TestCase):
    """验证从干净仓库复现实验的命令参数保持固定。"""

    def test_make_command_reuses_schedviz_long_demo(self) -> None:
        command = REPORT.build_make_command("sjf", 1)

        self.assertEqual("make", command[0])
        self.assertIn("schedviz", command)
        self.assertIn("SCHED_POLICY=sjf", command)
        self.assertIn("CPUS=1", command)
        self.assertIn("--seconds 60", command[-1])
        self.assertIn("--workers 6", command[-1])

    def test_unknown_policy_is_rejected(self) -> None:
        with self.assertRaisesRegex(REPORT.ExperimentError, "unsupported policy"):
            REPORT.build_make_command("cfs", 1)


if __name__ == "__main__":
    unittest.main()
