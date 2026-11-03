#!/usr/bin/env python3
"""Merge benchmark summaries from multiple solver versions by case."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


def load_summary(path: Path) -> dict[tuple[str, str], dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    return {(row["category"], row["case"]): row for row in rows}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--experiments-dir", default="data/experiments")
    parser.add_argument("--output", default="data/experiments/comparison.csv")
    args = parser.parse_args()

    root = Path(args.experiments_dir)
    summaries = {
        "baseline": load_summary(root / "baseline_summary.csv"),
        "degree_only": load_summary(root / "degree_only_summary.csv"),
        "propagation": load_summary(root / "propagation_summary.csv"),
        "mrv": load_summary(root / "mrv_summary.csv"),
    }
    keys = sorted(set().union(*(summary.keys() for summary in summaries.values())))

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow([
            "category",
            "case",
            "baseline_ms",
            "degree_only_ms",
            "propagation_ms",
            "mrv_ms",
            "baseline_nodes",
            "degree_only_nodes",
            "propagation_nodes",
            "mrv_nodes",
            "mrv_vs_propagation_node_reduction",
        ])
        for category, case in keys:
            base = summaries["baseline"].get((category, case), {})
            degree_only = summaries["degree_only"].get((category, case), {})
            propagation = summaries["propagation"].get((category, case), {})
            mrv = summaries["mrv"].get((category, case), {})

            reduction = ""
            if propagation.get("nodes") and mrv.get("nodes"):
                p_nodes = int(propagation["nodes"])
                m_nodes = int(mrv["nodes"])
                if p_nodes > 0:
                    reduction = f"{(p_nodes - m_nodes) / p_nodes:.6f}"

            writer.writerow([
                category,
                case,
                base.get("median_ms", ""),
                degree_only.get("median_ms", ""),
                propagation.get("median_ms", ""),
                mrv.get("median_ms", ""),
                base.get("nodes", ""),
                degree_only.get("nodes", ""),
                propagation.get("nodes", ""),
                mrv.get("nodes", ""),
                reduction,
            ])

    print(output)


if __name__ == "__main__":
    main()
