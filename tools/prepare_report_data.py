#!/usr/bin/env python3
"""Prepare compact, traceable CSV files used by report.pdf figures."""

from __future__ import annotations

import csv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXPERIMENTS = ROOT / "data" / "experiments"
FIGURES = ROOT / "reports" / "figures"


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def write_csv(path: Path, fields: list[str], rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    FIGURES.mkdir(parents=True, exist_ok=True)

    # Coverage counts come from the deterministic test suite implemented in
    # tests/test_runner.cpp. They are not benchmark samples.
    coverage = [
        {"test_group": "Official samples", "cases": "3", "label": "3"},
        {"test_group": "Parser cases", "cases": "13", "label": "13"},
        {"test_group": "Model checks", "cases": "8", "label": "8"},
        {"test_group": "Validator accepts", "cases": "5", "label": "5"},
        {"test_group": "Validator rejects", "cases": "15", "label": "15"},
        {"test_group": "CLI black box", "cases": "10", "label": "10"},
        {"test_group": "Exhaustive 2x2", "cases": "625", "label": "625"},
        {"test_group": "Exhaustive 2x3", "cases": "15625", "label": "15625"},
        {"test_group": "Random 2x2-3x4", "cases": "1000", "label": "1000"},
        {"test_group": "Random 3x3-3x4", "cases": "3000", "label": "3000"},
        {"test_group": "Planted feasible", "cases": "200", "label": "200"},
        {"test_group": "Propagation fixtures", "cases": "2", "label": "2"},
        {"test_group": "Required geometry", "cases": "9", "label": "9"},
    ]
    write_csv(FIGURES / "coverage.csv", ["test_group", "cases", "label"], coverage)

    comparison = read_csv(EXPERIMENTS / "comparison.csv")
    by_key = {(row["category"], row["case"]): row for row in comparison}
    selected = [
        ("planted_feasible", "n10_m10", "Planted 10"),
        ("planted_feasible", "n25_m25", "Planted 25"),
        ("planted_feasible", "n50_m50", "Planted 50"),
        ("conflict_heavy", "n50_m50", "Conflict 50"),
        ("dense_all_positive", "n50_m50", "Dense 50"),
    ]

    node_rows = []
    for category, case, label in selected:
        row = by_key[(category, case)]
        node_rows.append({
            "case": label,
            "degree_only": row["degree_only_nodes"],
            "degree_conflict": row["propagation_nodes"],
            "mrv": row["mrv_nodes"],
        })
    write_csv(
        FIGURES / "node_comparison.csv",
        ["case", "degree_only", "degree_conflict", "mrv"],
        node_rows,
    )

    runtime_rows = []
    for category, case, label in selected:
        row = by_key[(category, case)]
        runtime_rows.append({
            "case": label,
            "median_ms": row["mrv_ms"],
            "label": f"{float(row['mrv_ms']):.3f}",
        })
    write_csv(
        FIGURES / "runtime_comparison.csv",
        ["case", "median_ms", "label"],
        runtime_rows,
    )

    summary = read_csv(EXPERIMENTS / "mrv_summary.csv")
    summary_by_key = {(row["category"], row["case"]): row for row in summary}
    propagation_rows = []
    for category, case, label in selected:
        row = summary_by_key[(category, case)]
        propagation_rows.append({
            "case": label,
            "forced_select": row["forced_selections"],
            "forced_reject": row["forced_rejections"],
            "conflict_reject": row["conflict_rejections"],
        })
    write_csv(
        FIGURES / "propagation_activity.csv",
        ["case", "forced_select", "forced_reject", "conflict_reject"],
        propagation_rows,
    )

    # Fail early if a future benchmark rename breaks a figure silently.
    required = {
        (row["category"], row["case"])
        for row in comparison
    }
    for category, case, _ in selected:
        if (category, case) not in required:
            raise RuntimeError(f"Missing comparison case: {category}/{case}")
    for row in node_rows:
        if not row["mrv"]:
            raise RuntimeError(f"Missing MRV node count for {row['case']}")


if __name__ == "__main__":
    main()
