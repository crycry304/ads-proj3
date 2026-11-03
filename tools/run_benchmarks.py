#!/usr/bin/env python3
"""Run deterministic benchmark cases and aggregate repeated measurements."""

from __future__ import annotations

import argparse
import csv
import statistics
import subprocess
from pathlib import Path


METRIC_FIELDS = [
    "nodes",
    "backtracks",
    "propagation_events",
    "forced_selections",
    "forced_rejections",
    "conflict_rejections",
    "max_depth",
]

STABLE_FIELDS = [
    "n",
    "m",
    "connectors",
    "candidate_edges",
    "conflict_pairs",
    "found",
    *METRIC_FIELDS,
]


def run_case(executable: Path, input_path: Path, repeat: int,
             timeout: float) -> tuple[str, list[dict[str, str]], str]:
    command = [str(executable), "--repeat", str(repeat), str(input_path)]
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return "timeout", [], f"exceeded {timeout:g}s"

    if completed.returncode != 0:
        return "error", [], completed.stderr.strip()

    reader = csv.DictReader(completed.stdout.splitlines())
    rows = list(reader)
    if not rows:
        return "error", [], "benchmark produced no CSV rows"
    if len(rows) != repeat:
        return (
            "error",
            [],
            f"expected {repeat} repetitions, received {len(rows)}",
        )
    for field in STABLE_FIELDS:
        values = {row.get(field, "") for row in rows}
        if len(values) != 1:
            return (
                "error",
                [],
                f"non-deterministic {field}: {sorted(values)}",
            )
    return "pass", rows, ""


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark-exe", required=True)
    parser.add_argument("--cases-dir", default="data/generated")
    parser.add_argument("--output-dir", default="data/experiments")
    parser.add_argument("--label", default="current")
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    benchmark = Path(args.benchmark_exe).resolve()
    cases_dir = Path(args.cases_dir).resolve()
    output_dir = Path(args.output_dir).resolve()
    raw_dir = output_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)

    manifest_path = cases_dir / "manifest.csv"
    with manifest_path.open(encoding="utf-8", newline="") as stream:
        manifest = list(csv.DictReader(stream))

    raw_rows: list[dict[str, str]] = []
    status_rows: list[dict[str, str]] = []
    grouped: dict[tuple[str, str], list[dict[str, str]]] = {}

    for record in manifest:
        category = record["category"]
        case = record["case"]
        input_path = cases_dir / record["input"]
        status, rows, detail = run_case(
            benchmark, input_path, args.repeat, args.timeout
        )

        status_rows.append({
            "category": category,
            "case": case,
            "status": status,
            "detail": detail,
        })
        if status != "pass":
            continue

        for row in rows:
            row["category"] = category
            row["status"] = status
            raw_rows.append(row)
        grouped[(category, case)] = rows

    raw_path = raw_dir / f"{args.label}.csv"
    raw_fields = [
        "category",
        "status",
        "case",
        "n",
        "m",
        "connectors",
        "candidate_edges",
        "conflict_pairs",
        "found",
        *METRIC_FIELDS,
        "wall_ms",
    ]
    with raw_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=raw_fields)
        writer.writeheader()
        writer.writerows(raw_rows)

    summary_path = output_dir / f"{args.label}_summary.csv"
    with summary_path.open("w", encoding="utf-8", newline="") as stream:
        fields = [
            "category",
            "case",
            "samples",
            "found",
            *METRIC_FIELDS,
            "median_ms",
            "mean_ms",
            "max_ms",
        ]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for (category, case), rows in sorted(grouped.items()):
            times = [float(row["wall_ms"]) for row in rows]
            first = rows[0]
            writer.writerow({
                "category": category,
                "case": case,
                "samples": len(rows),
                "found": first["found"],
                **{field: first[field] for field in METRIC_FIELDS},
                "median_ms": f"{statistics.median(times):.6f}",
                "mean_ms": f"{statistics.mean(times):.6f}",
                "max_ms": f"{max(times):.6f}",
            })

    status_path = output_dir / f"{args.label}_status.csv"
    with status_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=["category", "case", "status", "detail"]
        )
        writer.writeheader()
        writer.writerows(status_rows)

    failures = [row for row in status_rows if row["status"] != "pass"]
    print(f"raw: {raw_path}")
    print(f"summary: {summary_path}")
    print(f"status: {status_path}")
    if failures:
        print(f"{len(failures)} case(s) failed or timed out")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
