#!/usr/bin/env python3
"""Compare VMRESULT lines produced by the four xv6 vmbench branches."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path

REQUIRED_VARIANTS = ("pristine", "lazy", "cow", "main")
RESULT_PREFIX = "VMRESULT "
PAIR_RE = re.compile(r"(?P<key>[A-Za-z0-9_]+)=(?P<value>[^\s]+)")


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

    ordered = sorted(expected)
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


def print_markdown(rows: list[dict[str, int | str]]) -> None:
    columns = (
        "scenario",
        "variant",
        "range_materialized",
        "range_copy_work",
        "range_total_page_work",
        "saved_materialized_vs_pristine",
        "saved_copy_vs_pristine",
        "saved_total_page_work_vs_pristine",
        "range_hole_scan",
    )
    print("| " + " | ".join(columns) + " |")
    print("| " + " | ".join("---" for _ in columns) + " |")
    for row in rows:
        print("| " + " | ".join(str(row[column]) for column in columns) + " |")


def write_csv(path: Path, rows: list[dict[str, int | str]]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


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
    parser.add_argument("--csv", type=Path, help="also write the full comparison as CSV")
    args = parser.parse_args()

    assignments = dict(args.logs)
    missing = [variant for variant in REQUIRED_VARIANTS if variant not in assignments]
    if missing:
        parser.error(f"missing variants: {', '.join(missing)}")

    try:
        data = {variant: parse_log(assignments[variant]) for variant in REQUIRED_VARIANTS}
        scenarios = validate_inputs(data)
        rows = build_rows(data, scenarios)
    except (OSError, ValueError) as error:
        print(f"vmbench_compare: {error}", file=sys.stderr)
        return 1

    print_markdown(rows)
    if args.csv:
        write_csv(args.csv, rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
