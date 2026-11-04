#!/usr/bin/env python3
"""Run fixed adversarial probes and classify timeout as pending evidence."""

from __future__ import annotations

import argparse
import csv
import subprocess
import time
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark-exe", required=True)
    parser.add_argument("--cases-dir", default="data/stress")
    parser.add_argument("--output-dir", default="data/stress-results")
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    benchmark = Path(args.benchmark_exe).resolve()
    cases_dir = Path(args.cases_dir).resolve()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    with (cases_dir / "manifest.csv").open(
        encoding="utf-8", newline=""
    ) as stream:
        cases = list(csv.DictReader(stream))

    results: list[dict[str, str]] = []
    failed = False
    for record in cases:
        input_path = cases_dir / record["input"]
        command = [str(benchmark), "--repeat", "1", str(input_path)]
        start = time.monotonic()
        status = "pass"
        detail = ""
        try:
            completed = subprocess.run(
                command,
                capture_output=True,
                text=True,
                timeout=args.timeout,
                check=False,
            )
        except subprocess.TimeoutExpired:
            status = "pending"
            detail = f"timeout after {args.timeout:g}s"
            completed = None
        elapsed_ms = 1000.0 * (time.monotonic() - start)

        if completed is not None:
            if completed.returncode != 0:
                status = "error"
                detail = completed.stderr.strip()
                failed = True
            elif not completed.stdout.strip():
                status = "error"
                detail = "benchmark produced no CSV rows"
                failed = True

        results.append({
            "category": record["category"],
            "case": record["case"],
            "seed": record["seed"],
            "density": record["density"],
            "timeout_s": f"{args.timeout:g}",
            "status": status,
            "elapsed_ms": f"{elapsed_ms:.3f}",
            "detail": detail,
        })

    status_path = output_dir / "stress_status.csv"
    with status_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=[
                "category",
                "case",
                "seed",
                "density",
                "timeout_s",
                "status",
                "elapsed_ms",
                "detail",
            ],
        )
        writer.writeheader()
        writer.writerows(results)

    for result in results:
        print(
            f"{result['category']}/{result['case']}: "
            f"{result['status']} ({result['detail'] or 'completed'})"
        )
    print(f"status: {status_path}")
    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
