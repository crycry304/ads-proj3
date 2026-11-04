#!/usr/bin/env python3
"""Generate the deterministic adversarial stress corpus."""

from __future__ import annotations

import argparse
import csv
import random
from pathlib import Path

from generate_cases import planted_feasible, write_case


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", default="data/stress")
    parser.add_argument("--seed", type=int, default=20261104)
    parser.add_argument("--density", type=float, default=0.55)
    args = parser.parse_args()

    output = Path(args.output_dir)
    category = "adversarial_planted_dense"
    case = "n50_m50_d055"
    relative = Path(category) / f"{case}.in"
    grid = planted_feasible(50, 50, args.density, random.Random(args.seed))
    write_case(output / relative, grid)

    manifest = output / "manifest.csv"
    with manifest.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["category", "case", "input", "seed", "density"])
        writer.writerow([
            category,
            case,
            str(relative).replace("\\", "/"),
            args.seed,
            f"{args.density:.2f}",
        ])


if __name__ == "__main__":
    main()
