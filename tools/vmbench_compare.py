#!/usr/bin/env python3
"""Compare VMRESULT lines and render CSV/Markdown/SVG reports."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from html import escape
from pathlib import Path

REQUIRED_VARIANTS = ("pristine", "lazy", "cow", "main")
SCENARIO_ORDER = (
    "reserve-only",
    "sparse-touch",
    "dense-touch",
    "fork-exit",
    "fork-exec",
    "fork-write-1of64",
    "fork-write-1of4",
    "fork-write-all",
    "sparse-fork-exec",
)
RESULT_PREFIX = "VMRESULT "
PAIR_RE = re.compile(r"(?P<key>[A-Za-z0-9_]+)=(?P<value>[^\s]+)")
VARIANT_COLORS = {
    "pristine": "#4e79a7",
    "lazy": "#59a14f",
    "cow": "#f28e2b",
    "main": "#e15759",
}


def parse_log(path: Path) -> dict[str, dict[str, int | str]]:
    results: dict[str, dict[str, int | str]] = {}
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        marker = raw_line.find(RESULT_PREFIX)
        if marker < 0:
            continue
        line = raw_line[marker + len(RESULT_PREFIX) :]
        row: dict[str, int | str] = {}
        for match in PAIR_RE.finditer(line):
            key = match.group("key")
            value = match.group("value")
            try:
                row[key] = int(value, 0)
            except ValueError:
                row[key] = value
        scenario = row.get("scenario")
        if not isinstance(scenario, str):
            raise ValueError(f"{path}: malformed VMRESULT without scenario: {raw_line}")
        if scenario in results:
            raise ValueError(f"{path}: duplicate scenario {scenario!r}")
        results[scenario] = row
    if not results:
        raise ValueError(f"{path}: no VMRESULT lines found")
    return results


def metric(row: dict[str, int | str], key: str) -> int:
    value = row.get(key)
    if not isinstance(value, int):
        raise ValueError(f"missing integer metric {key!r} in scenario {row.get('scenario')!r}")
    return value


def derived(row: dict[str, int | str]) -> dict[str, int]:
    materialized = metric(row, "range_eager_alloc") + metric(row, "range_lazy_alloc")
    copied = metric(row, "range_fork_copy") + metric(row, "range_cow_copy")
    return {
        "range_materialized": materialized,
        "range_copy_work": copied,
        "range_total_page_work": materialized + copied,
        "range_hole_scan": metric(row, "range_fork_scan") - metric(row, "range_fork_present"),
    }


def validate_inputs(data: dict[str, dict[str, dict[str, int | str]]]) -> list[str]:
    scenario_sets = {variant: set(rows) for variant, rows in data.items()}
    expected = scenario_sets[REQUIRED_VARIANTS[0]]
    for variant, scenarios in scenario_sets.items():
        if scenarios != expected:
            missing = sorted(expected - scenarios)
            extra = sorted(scenarios - expected)
            raise ValueError(f"{variant}: scenario mismatch; missing={missing}, extra={extra}")

    ordered = [scenario for scenario in SCENARIO_ORDER if scenario in expected]
    ordered.extend(sorted(expected - set(ordered)))
    for scenario in ordered:
        pages = {metric(data[variant][scenario], "pages") for variant in REQUIRED_VARIANTS}
        if len(pages) != 1:
            raise ValueError(f"{scenario}: branches used different page counts: {sorted(pages)}")
    return ordered


def build_rows(
    data: dict[str, dict[str, dict[str, int | str]]], scenarios: list[str]
) -> list[dict[str, int | str]]:
    output: list[dict[str, int | str]] = []
    for scenario in scenarios:
        pristine = derived(data["pristine"][scenario])
        for variant in REQUIRED_VARIANTS:
            raw = data[variant][scenario]
            values = derived(raw)
            output.append(
                {
                    "scenario": scenario,
                    "variant": variant,
                    "pages": metric(raw, "pages"),
                    **values,
                    "saved_materialized_vs_pristine": pristine["range_materialized"]
                    - values["range_materialized"],
                    "saved_copy_vs_pristine": pristine["range_copy_work"]
                    - values["range_copy_work"],
                    "saved_total_page_work_vs_pristine": pristine["range_total_page_work"]
                    - values["range_total_page_work"],
                    "range_fork_shared": metric(raw, "range_fork_shared"),
                    "range_cow_mark": metric(raw, "range_cow_mark"),
                }
            )
    return output


TABLE_COLUMNS = (
    "scenario",
    "variant",
    "range_materialized",
    "range_copy_work",
    "range_total_page_work",
    "saved_materialized_vs_pristine",
    "saved_copy_vs_pristine",
    "saved_total_page_work_vs_pristine",
    "range_hole_scan",
    "range_fork_shared",
    "range_cow_mark",
)


def markdown_text(rows: list[dict[str, int | str]]) -> str:
    lines = [
        "| " + " | ".join(TABLE_COLUMNS) + " |",
        "| " + " | ".join("---" for _ in TABLE_COLUMNS) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(str(row[column]) for column in TABLE_COLUMNS) + " |")
    return "\n".join(lines) + "\n"


def write_csv(path: Path, rows: list[dict[str, int | str]]) -> None:
    if not rows:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def _nice_ticks(low: float, high: float, count: int = 5) -> list[float]:
    if high <= low:
        high = low + 1
    raw_step = (high - low) / count
    magnitude = 10 ** max(0, len(str(int(raw_step))) - 1) if raw_step >= 1 else 1
    normalized = raw_step / magnitude
    if normalized <= 1:
        step = 1 * magnitude
    elif normalized <= 2:
        step = 2 * magnitude
    elif normalized <= 5:
        step = 5 * magnitude
    else:
        step = 10 * magnitude
    start = int(low // step) * step
    ticks: list[float] = []
    value = start
    while value <= high + step:
        if value >= low:
            ticks.append(value)
        value += step
    return ticks or [low, high]


def write_svg_chart(
    path: Path,
    rows: list[dict[str, int | str]],
    scenarios: list[str],
    metric_key: str,
    title: str,
    y_label: str,
) -> None:
    width, height = 1280, 760
    left, right, top, bottom = 100, 40, 80, 210
    plot_w = width - left - right
    plot_h = height - top - bottom

    indexed = {(str(row["scenario"]), str(row["variant"])): row for row in rows}
    values = [
        int(indexed[(scenario, variant)][metric_key])
        for scenario in scenarios
        for variant in REQUIRED_VARIANTS
    ]
    low = min(0, min(values))
    high = max(values)
    if high == low:
        high = low + 1
    padding = max(1.0, (high - low) * 0.08)
    y_min = low - (padding if low < 0 else 0)
    y_max = high + padding
    ticks = _nice_ticks(y_min, y_max)
    y_min = min(y_min, min(ticks))
    y_max = max(y_max, max(ticks))

    def x_pos(index: int) -> float:
        if len(scenarios) == 1:
            return left + plot_w / 2
        return left + index * plot_w / (len(scenarios) - 1)

    def y_pos(value: float) -> float:
        return top + (y_max - value) * plot_h / (y_max - y_min)

    svg: list[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        '<style>text{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;fill:#222}.grid{stroke:#ddd;stroke-width:1}.axis{stroke:#333;stroke-width:1.5}.line{fill:none;stroke-width:3}.point{stroke:white;stroke-width:1.5}</style>',
        f'<text x="{width/2}" y="38" text-anchor="middle" font-size="24" font-weight="600">{escape(title)}</text>',
    ]

    for tick in ticks:
        y = y_pos(tick)
        svg.append(
            f'<line class="grid" x1="{left}" y1="{y:.2f}" x2="{width-right}" y2="{y:.2f}"/>'
        )
        label = str(int(tick)) if float(tick).is_integer() else f"{tick:.2f}"
        svg.append(
            f'<text x="{left-12}" y="{y+5:.2f}" text-anchor="end" font-size="14">{label}</text>'
        )

    svg.append(
        f'<line class="axis" x1="{left}" y1="{top}" x2="{left}" y2="{top+plot_h}"/>'
    )
    svg.append(
        f'<line class="axis" x1="{left}" y1="{top+plot_h}" x2="{width-right}" y2="{top+plot_h}"/>'
    )
    svg.append(
        f'<text x="24" y="{top+plot_h/2}" text-anchor="middle" font-size="16" transform="rotate(-90 24 {top+plot_h/2})">{escape(y_label)}</text>'
    )

    for index, scenario in enumerate(scenarios):
        x = x_pos(index)
        svg.append(
            f'<line class="grid" x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{top+plot_h}"/>'
        )
        svg.append(
            f'<text x="{x:.2f}" y="{top+plot_h+24}" text-anchor="end" font-size="13" transform="rotate(-38 {x:.2f} {top+plot_h+24})">{escape(scenario)}</text>'
        )

    for variant in REQUIRED_VARIANTS:
        color = VARIANT_COLORS[variant]
        points = [
            (x_pos(index), y_pos(int(indexed[(scenario, variant)][metric_key])))
            for index, scenario in enumerate(scenarios)
        ]
        point_text = " ".join(f"{x:.2f},{y:.2f}" for x, y in points)
        svg.append(f'<polyline class="line" stroke="{color}" points="{point_text}"/>')
        for (x, y), scenario in zip(points, scenarios):
            value = indexed[(scenario, variant)][metric_key]
            svg.append(
                f'<circle class="point" fill="{color}" cx="{x:.2f}" cy="{y:.2f}" r="5"><title>{escape(variant)} / {escape(scenario)}: {value}</title></circle>'
            )

    legend_y = height - 38
    legend_x = left
    for variant in REQUIRED_VARIANTS:
        color = VARIANT_COLORS[variant]
        svg.append(
            f'<line x1="{legend_x}" y1="{legend_y}" x2="{legend_x+34}" y2="{legend_y}" stroke="{color}" stroke-width="4"/>'
        )
        svg.append(
            f'<text x="{legend_x+43}" y="{legend_y+5}" font-size="15">{escape(variant)}</text>'
        )
        legend_x += 190

    svg.append("</svg>")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(svg) + "\n", encoding="utf-8")


def write_svg_reports(
    directory: Path, rows: list[dict[str, int | str]], scenarios: list[str]
) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    specs = (
        ("01-materialized-pages.svg", "range_materialized", "用户数据页物化数量", "pages"),
        ("02-copy-work-pages.svg", "range_copy_work", "fork/COW 整页复制工作", "pages"),
        ("03-total-page-work.svg", "range_total_page_work", "物化与复制总页级工作", "pages"),
        (
            "04-saved-total-vs-pristine.svg",
            "saved_total_page_work_vs_pristine",
            "相对 pristine 避免的总页级工作",
            "saved pages",
        ),
        ("05-fork-shared-pages.svg", "range_fork_shared", "fork 后共享物理页数量", "pages"),
        ("06-hole-scan-pages.svg", "range_hole_scan", "fork 扫描但未映射的虚拟页", "pages"),
    )
    for filename, key, title, label in specs:
        write_svg_chart(directory / filename, rows, scenarios, key, title, label)


def parse_assignment(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("expected VARIANT=LOG_PATH")
    variant, path = value.split("=", 1)
    if variant not in REQUIRED_VARIANTS:
        raise argparse.ArgumentTypeError(
            f"variant must be one of {', '.join(REQUIRED_VARIANTS)}"
        )
    return variant, Path(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "logs",
        nargs=4,
        type=parse_assignment,
        metavar="VARIANT=LOG",
        help="one log for each of pristine, lazy, cow, and main",
    )
    parser.add_argument("--csv", type=Path, help="write the full comparison as CSV")
    parser.add_argument("--markdown", type=Path, help="write the Markdown comparison table")
    parser.add_argument("--svg-dir", type=Path, help="write SVG line charts into this directory")
    args = parser.parse_args()

    assignments = dict(args.logs)
    missing = [variant for variant in REQUIRED_VARIANTS if variant not in assignments]
    if missing:
        parser.error(f"missing variants: {', '.join(missing)}")

    try:
        data = {
            variant: parse_log(assignments[variant]) for variant in REQUIRED_VARIANTS
        }
        scenarios = validate_inputs(data)
        rows = build_rows(data, scenarios)
        markdown = markdown_text(rows)
        if args.csv:
            write_csv(args.csv, rows)
        if args.markdown:
            args.markdown.parent.mkdir(parents=True, exist_ok=True)
            args.markdown.write_text(markdown, encoding="utf-8")
        if args.svg_dir:
            write_svg_reports(args.svg_dir, rows, scenarios)
    except (OSError, ValueError) as error:
        print(f"vmbench_compare: {error}", file=sys.stderr)
        return 1

    print(markdown, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
