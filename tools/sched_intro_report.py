#!/usr/bin/env python3
"""运行 xv6 调度入门实验，并把 schedviz trace 收敛为可比较报告。"""

from __future__ import annotations

import argparse
import csv
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Mapping, Sequence

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ARTIFACT_DIR = REPO_ROOT / "artifacts" / "schedviz"
POLICIES = ("fifo", "sjf", "stcf", "rr")
EXPERIMENT_SECONDS = 60
EXPERIMENT_WORKERS = 6
LEGEND_PATTERN = re.compile(
    r"^\s*(?P<glyph>[A-Z]) = worker(?P<worker>\d+) pid=(?P<pid>\d+) "
    r"hint=(?P<hint>\d+) weight=(?P<weight>\d+)\s*$"
)


class ExperimentError(RuntimeError):
    """表示命令、trace 协议或调度语义证据不满足实验契约。"""


@dataclass(frozen=True)
class TraceEvent:
    seq: int
    timestamp: int
    tick: int
    cpu: int
    pid: int
    event_type: str
    reason: str
    runtime: int
    dispatches: int
    remaining: int
    name: str


@dataclass(frozen=True)
class TraceData:
    version: int
    policy: str
    cpus: int
    expected_events: int
    dropped: int
    events: tuple[TraceEvent, ...]


@dataclass(frozen=True)
class WorkerLegend:
    glyph: str
    worker: int
    pid: int
    hint: int
    weight: int


@dataclass(frozen=True)
class WorkerMetric:
    policy: str
    glyph: str
    worker: int
    pid: int
    hint: int
    arrival_tick: int
    first_run_tick: int
    completion_tick: int
    response_ticks: int
    turnaround_ticks: int
    dispatches: int


@dataclass(frozen=True)
class PolicyResult:
    policy: str
    command: tuple[str, ...]
    log_path: Path
    trace_path: Path
    svg_path: Path
    evidence: str
    metrics: tuple[WorkerMetric, ...]


def _parse_key_values(line: str) -> dict[str, str]:
    """解析由空格分隔的 `key=value` 字段。"""

    return {
        key: value
        for token in line.split()
        if "=" in token
        for key, value in (token.split("=", 1),)
    }


def parse_trace_text(text: str) -> TraceData:
    """严格解析完整 SCHEDTRACE 协议，遇到丢事件或不完整协议即失败。"""

    header: dict[str, str] | None = None
    events: list[TraceEvent] = []
    done = False
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("SCHEDTRACE version="):
            header = _parse_key_values(line)
        elif line.startswith("SCHEDTRACE event "):
            fields = _parse_key_values(line)
            try:
                events.append(
                    TraceEvent(
                        seq=int(fields["seq"]),
                        timestamp=int(fields["ts"]),
                        tick=int(fields["tick"]),
                        cpu=int(fields["cpu"]),
                        pid=int(fields["pid"]),
                        event_type=fields["type"],
                        reason=fields["reason"],
                        runtime=int(fields["runtime"]),
                        dispatches=int(fields["dispatches"]),
                        remaining=int(fields["remaining"]),
                        name=fields["name"],
                    )
                )
            except (KeyError, ValueError) as error:
                raise ExperimentError(f"invalid SCHEDTRACE event: {line}") from error
        elif line == "SCHEDTRACE done status=0":
            done = True

    if header is None:
        raise ExperimentError("missing SCHEDTRACE header")
    try:
        version = int(header["version"])
        policy = header["policy"]
        cpus = int(header["cpus"])
        expected_events = int(header["events"])
        dropped = int(header["dropped"])
    except (KeyError, ValueError) as error:
        raise ExperimentError(f"invalid SCHEDTRACE header: {header}") from error

    if not done:
        raise ExperimentError("missing successful SCHEDTRACE done marker")
    if dropped != 0:
        raise ExperimentError(f"trace dropped {dropped} event(s)")
    if expected_events != len(events):
        raise ExperimentError(
            f"trace event count mismatch: header={expected_events} parsed={len(events)}"
        )
    if any(current.seq <= previous.seq for previous, current in zip(events, events[1:])):
        raise ExperimentError("trace sequence is not strictly increasing")
    if policy not in POLICIES:
        raise ExperimentError(f"unexpected policy in trace: {policy}")
    if cpus < 1:
        raise ExperimentError(f"invalid CPU count in trace: {cpus}")
    return TraceData(version, policy, cpus, expected_events, dropped, tuple(events))


def parse_trace_file(path: Path) -> TraceData:
    """读取并解析 UTF-8 trace 文件。"""

    return parse_trace_text(path.read_text(encoding="utf-8"))


def parse_legend(text: str) -> dict[int, WorkerLegend]:
    """从 schedviz 控制台日志提取 worker 到 PID 的稳定映射。"""

    legends: dict[int, WorkerLegend] = {}
    for line in text.splitlines():
        match = LEGEND_PATTERN.match(line)
        if match is None:
            continue
        legend = WorkerLegend(
            glyph=match.group("glyph"),
            worker=int(match.group("worker")),
            pid=int(match.group("pid")),
            hint=int(match.group("hint")),
            weight=int(match.group("weight")),
        )
        if legend.pid in legends:
            raise ExperimentError(f"duplicate legend pid: {legend.pid}")
        legends[legend.pid] = legend
    if not legends:
        raise ExperimentError("schedviz log does not contain worker legend")
    return legends


def derive_metrics(
    trace: TraceData,
    legends: Mapping[int, WorkerLegend],
    strict_single_cpu: bool = True,
) -> tuple[WorkerMetric, ...]:
    """由 RUN_START/RUN_STOP 和场景 barrier 推导响应、周转与调度次数。

    FIFO、SJF、RR 的 long demo 同时释放 worker，统一用 session 首个 RUN_START tick
    作为 arrival 原点。单核 STCF 先释放长任务，再释放短任务；第一次
    `STCF_SHORTER_TASK` 停止点是短任务到达边界。多核只做 lifecycle smoke，不要求
    出现单核抢占事件，因此把所有 worker 的 arrival 归一化到 session 原点。
    """

    starts = [event for event in trace.events if event.event_type == "RUN_START"]
    if not starts:
        raise ExperimentError("trace does not contain RUN_START")
    origin_tick = min(event.tick for event in starts)
    first_pid = starts[0].pid
    stcf_arrival = next(
        (
            event.tick
            for event in trace.events
            if event.event_type == "RUN_STOP"
            and event.reason == "STCF_SHORTER_TASK"
        ),
        None,
    )

    metrics: list[WorkerMetric] = []
    for legend in sorted(legends.values(), key=lambda item: item.worker):
        worker_events = [event for event in trace.events if event.pid == legend.pid]
        worker_starts = [event for event in worker_events if event.event_type == "RUN_START"]
        exits = [
            event
            for event in worker_events
            if event.event_type == "RUN_STOP" and event.reason == "EXIT"
        ]
        if not worker_starts or not exits:
            raise ExperimentError(
                f"worker{legend.worker} pid={legend.pid} lacks complete RUN_START/EXIT lifecycle"
            )
        if strict_single_cpu and trace.policy == "stcf" and legend.pid != first_pid:
            if stcf_arrival is None:
                raise ExperimentError("STCF trace lacks shorter-task preemption boundary")
            arrival_tick = stcf_arrival
        else:
            arrival_tick = origin_tick
        first_run_tick = worker_starts[0].tick
        completion = exits[-1]
        response = first_run_tick - arrival_tick
        turnaround = completion.tick - arrival_tick
        if response < 0 or turnaround < response:
            raise ExperimentError(
                f"invalid lifecycle ticks for worker{legend.worker}: "
                f"arrival={arrival_tick} first={first_run_tick} completion={completion.tick}"
            )
        metrics.append(
            WorkerMetric(
                trace.policy,
                legend.glyph,
                legend.worker,
                legend.pid,
                legend.hint,
                arrival_tick,
                first_run_tick,
                completion.tick,
                response,
                turnaround,
                completion.dispatches,
            )
        )
    return tuple(metrics)


def validate_policy_evidence(
    trace: TraceData,
    metrics: Sequence[WorkerMetric],
    strict_single_cpu: bool,
) -> str:
    """校验四类教材策略的关键证据；SMP 不套用单核完成顺序判据。"""

    if not metrics:
        raise ExperimentError(f"{trace.policy}: no worker metrics")
    if not strict_single_cpu:
        return f"SMP smoke: {len(metrics)} workers completed, dropped=0"

    completion_order = [
        metric.worker
        for metric in sorted(metrics, key=lambda item: (item.completion_tick, item.worker))
    ]
    if trace.policy == "fifo":
        expected = [metric.worker for metric in sorted(metrics, key=lambda item: item.worker)]
        if completion_order != expected:
            raise ExperimentError(
                f"FIFO completion order mismatch: got={completion_order} expected={expected}"
            )
        return f"completion order follows enqueue order {completion_order}"
    if trace.policy == "sjf":
        expected = [
            metric.worker
            for metric in sorted(metrics, key=lambda item: (item.hint, item.worker))
        ]
        if completion_order != expected:
            raise ExperimentError(
                f"SJF completion order mismatch: got={completion_order} expected={expected}"
            )
        return f"completion order follows burst hint {completion_order}"
    if trace.policy == "stcf":
        preemptions = [
            event
            for event in trace.events
            if event.event_type == "RUN_STOP"
            and event.reason == "STCF_SHORTER_TASK"
        ]
        long_worker = max(metrics, key=lambda item: item.hint)
        earlier_short = any(
            metric.worker != long_worker.worker
            and metric.completion_tick < long_worker.completion_tick
            for metric in metrics
        )
        if not preemptions or not earlier_short:
            raise ExperimentError("STCF trace does not prove shorter-task preemption")
        return (
            f"observed {len(preemptions)} shorter-task preemption(s); "
            f"worker{long_worker.worker} completed after a shorter worker"
        )
    if trace.policy == "rr":
        quantum_stops = [
            event
            for event in trace.events
            if event.event_type == "RUN_STOP" and event.reason == "RR_QUANTUM"
        ]
        if not quantum_stops or any(metric.dispatches < 2 for metric in metrics):
            raise ExperimentError("RR trace does not prove time-slice rotation")
        return (
            f"observed {len(quantum_stops)} RR quantum stop(s); "
            "every worker was dispatched at least twice"
        )
    raise ExperimentError(f"unsupported policy: {trace.policy}")


def build_make_command(policy: str, cpus: int) -> tuple[str, ...]:
    """构造复用现有 `make schedviz` 的可复制命令。"""

    if policy not in POLICIES:
        raise ExperimentError(f"unsupported policy: {policy}")
    if cpus < 1:
        raise ExperimentError(f"invalid CPU count: {cpus}")
    return (
        "make",
        "schedviz",
        f"SCHED_POLICY={policy}",
        f"CPUS={cpus}",
        "SCHEDVIZ_SUFFIX=-intro",
        f"SCHEDVIZ_ARGS=--plain --seconds {EXPERIMENT_SECONDS} "
        f"--workers {EXPERIMENT_WORKERS}",
    )


def _run(command: Sequence[str]) -> subprocess.CompletedProcess[str]:
    """在仓库根目录执行命令并合并捕获 stdout/stderr。"""

    return subprocess.run(
        command,
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def run_policy(policy: str, cpus: int, artifact_dir: Path) -> PolicyResult:
    """运行一个策略，保存日志并消费现有 schedviz trace/SVG 产物。"""

    command = build_make_command(policy, cpus)
    completed = _run(command)
    artifact_dir.mkdir(parents=True, exist_ok=True)
    log_path = artifact_dir / f"{policy}-cpu{cpus}-intro.log"
    log_path.write_text(f"$ {' '.join(command)}\n{completed.stdout}", encoding="utf-8")
    if completed.returncode != 0:
        raise ExperimentError(
            f"{policy}: command exited with {completed.returncode}; log={log_path}"
        )

    generated_trace = DEFAULT_ARTIFACT_DIR / f"{policy}-cpu{cpus}-intro.trace"
    generated_svg = DEFAULT_ARTIFACT_DIR / f"{policy}-cpu{cpus}-intro.svg"
    if not generated_trace.is_file() or not generated_svg.is_file():
        raise ExperimentError(f"{policy}: trace/SVG was not generated")
    trace_path = artifact_dir / generated_trace.name
    svg_path = artifact_dir / generated_svg.name
    if trace_path != generated_trace:
        shutil.copy2(generated_trace, trace_path)
    if svg_path != generated_svg:
        shutil.copy2(generated_svg, svg_path)

    trace = parse_trace_file(trace_path)
    if trace.policy != policy or trace.cpus != cpus:
        raise ExperimentError(
            f"{policy}: artifact header mismatch policy={trace.policy} cpus={trace.cpus}"
        )
    strict_single_cpu = cpus == 1
    metrics = derive_metrics(
        trace,
        parse_legend(completed.stdout),
        strict_single_cpu=strict_single_cpu,
    )
    evidence = validate_policy_evidence(trace, metrics, strict_single_cpu)
    return PolicyResult(
        policy, command, log_path, trace_path, svg_path, evidence, metrics
    )


def _git_revision() -> str:
    completed = _run(("git", "rev-parse", "HEAD"))
    return completed.stdout.strip() if completed.returncode == 0 else "unavailable"


def write_reports(
    artifact_dir: Path,
    cpus: int,
    results: Sequence[PolicyResult],
) -> tuple[Path, Path]:
    """将 worker 指标写入 CSV，并生成带复现命令和边界的 Markdown。"""

    csv_path = artifact_dir / f"intro-cpu{cpus}-metrics.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(
            (
                "policy",
                "glyph",
                "worker",
                "pid",
                "hint",
                "arrival_tick",
                "first_run_tick",
                "completion_tick",
                "response_ticks",
                "turnaround_ticks",
                "dispatches",
            )
        )
        for result in results:
            for metric in result.metrics:
                writer.writerow(tuple(metric.__dict__.values()))

    markdown_path = artifact_dir / f"intro-cpu{cpus}-summary.md"
    lines = [
        "# xv6 Scheduling Intro Report",
        "",
        f"- Revision: `{_git_revision()}`",
        f"- CPUs: `{cpus}`",
        f"- Generated at: `{datetime.now(timezone.utc).isoformat()}`",
        f"- Shared args: `--seconds {EXPERIMENT_SECONDS} --workers {EXPERIMENT_WORKERS}`",
        "",
        "## Policy evidence",
        "",
        "| Policy | Evidence |",
        "|---|---|",
        *(f"| `{result.policy}` | {result.evidence} |" for result in results),
        "",
        "## Worker metrics",
        "",
        "| Policy | Worker | Hint | Arrival | First run | Completion | Response | Turnaround | Dispatches |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for result in results:
        for metric in result.metrics:
            lines.append(
                f"| `{metric.policy}` | {metric.glyph}/worker{metric.worker} | "
                f"{metric.hint} | {metric.arrival_tick} | {metric.first_run_tick} | "
                f"{metric.completion_tick} | {metric.response_ticks} | "
                f"{metric.turnaround_ticks} | {metric.dispatches} |"
            )
    lines.extend(("", "## Reproduction commands", ""))
    for result in results:
        lines.extend(("```bash", " ".join(result.command), "```", ""))
    lines.extend(
        (
            "## Artifacts",
            "",
            *(
                f"- `{result.policy}`: `{result.log_path.name}`, "
                f"`{result.trace_path.name}`, `{result.svg_path.name}`"
                for result in results
            ),
            "",
            "## Measurement boundary",
            "",
            "- FIFO、SJF、RR 的 arrival 归一化为共同 barrier 后第一个 RUN_START tick。",
            "- 单核 STCF 的短任务 arrival 使用第一次 STCF_SHORTER_TASK 停止点；多核 smoke 统一使用 session 原点。",
            "- `CPUS=1` 验证教材完成顺序和 response/turnaround；SMP 只验证事件完整性、并行运行和资源回收。",
            "- burst hint 是实验输入，不表示内核能够预测未来 CPU burst；SJF/STCF 均为教学实现。",
            "",
        )
    )
    markdown_path.write_text("\n".join(lines), encoding="utf-8")
    return csv_path, markdown_path


def run_experiment(cpus: int, artifact_dir: Path) -> tuple[Path, Path]:
    """顺序运行 FIFO/SJF/STCF/RR，并生成统一报告。"""

    results = tuple(run_policy(policy, cpus, artifact_dir) for policy in POLICIES)
    return write_reports(artifact_dir, cpus, results)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run FIFO/SJF/STCF/RR schedviz experiments and build reports."
    )
    parser.add_argument("--cpus", type=int, default=1)
    parser.add_argument("--artifacts", type=Path, default=DEFAULT_ARTIFACT_DIR)
    args = parser.parse_args(argv)
    if args.cpus < 1:
        parser.error("--cpus must be positive")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        csv_path, markdown_path = run_experiment(args.cpus, args.artifacts)
    except ExperimentError as error:
        print(f"sched_intro_report: FAIL: {error}", file=sys.stderr)
        return 1
    print(f"sched_intro_report: wrote {csv_path} and {markdown_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
