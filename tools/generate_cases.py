#!/usr/bin/env python3
"""Generate deterministic correctness and performance cases for Recover the Design."""

from __future__ import annotations

import argparse
import csv
import random
from pathlib import Path


def write_case(path: Path, grid: list[list[int]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(f"{len(grid)} {len(grid[0])}\n")
        for row in grid:
            stream.write(" ".join(map(str, row)) + "\n")


def random_degrees(rows: int, cols: int, density: float, rng: random.Random) -> list[list[int]]:
    grid = [[0 for _ in range(cols)] for _ in range(rows)]
    for r in range(rows):
        for c in range(cols):
            if rng.random() < density:
                grid[r][c] = rng.randint(1, 4)
    return grid


def candidate_edges(mask: list[list[int]]) -> tuple[list[tuple[int, int, int, int]], list[list[bool]]]:
    rows = len(mask)
    cols = len(mask[0])
    edges: list[tuple[int, int, int, int]] = []
    for r in range(rows):
        positive = [c for c in range(cols) if mask[r][c]]
        edges.extend((r, c1, r, c2) for c1, c2 in zip(positive, positive[1:]))
    for c in range(cols):
        positive = [r for r in range(rows) if mask[r][c]]
        edges.extend((r1, c, r2, c) for r1, r2 in zip(positive, positive[1:]))

    # Build the actual horizontal/vertical intersection relation directly.
    conflicts: list[list[bool]] = [[False] * len(edges) for _ in range(len(edges))]
    for i, (r1, c1, r2, c2) in enumerate(edges):
        horizontal = r1 == r2
        for j, (s1, d1, s2, d2) in enumerate(edges):
            if i == j or horizontal == (s1 == s2):
                continue
            if horizontal:
                row, low, high = r1, c1, c2
                col, top, bottom = d1, s1, s2
            else:
                row, low, high = s1, d1, d2
                col, top, bottom = c1, r1, r2
            if low < col < high and top < row < bottom:
                conflicts[i][j] = True
    return edges, conflicts


def planted_feasible(rows: int, cols: int, density: float,
                     rng: random.Random) -> list[list[int]]:
    base = [[1 if rng.random() < density else 0 for _ in range(cols)]
            for _ in range(rows)]
    edges, conflicts = candidate_edges(base)
    order = list(range(len(edges)))
    rng.shuffle(order)
    selected: list[int] = []
    degree = [[0 for _ in range(cols)] for _ in range(rows)]
    for edge_id in order:
        if any(conflicts[edge_id][other] for other in selected):
            continue
        if rng.random() < 0.55:
            selected.append(edge_id)
            r1, c1, r2, c2 = edges[edge_id]
            degree[r1][c1] += 1
            degree[r2][c2] += 1
    return degree


def all_horizontal(rows: int, cols: int) -> list[list[int]]:
    return [[1 if c in (0, cols - 1) else 2 for c in range(cols)]
            for _ in range(rows)]


def odd_total_case(rows: int, cols: int, rng: random.Random) -> list[list[int]]:
    grid = planted_feasible(rows, cols, 0.55, rng)
    for r in range(rows):
        for c in range(cols):
            if 0 < grid[r][c] < 4:
                grid[r][c] += 1
                return grid
    grid[0][0] = 1
    return grid


def conflict_trap(rows: int, cols: int) -> list[list[int]]:
    grid = [[0 for _ in range(cols)] for _ in range(rows)]
    # Repeating independent crosses creates many horizontal/vertical
    # intersections while leaving every crossing point empty.
    for top in range(0, max(1, rows - 2), 4):
        for left in range(0, max(1, cols - 2), 4):
            if top + 2 < rows and left + 2 < cols:
                grid[top][left + 1] = 1
                grid[top + 1][left] = 1
                grid[top + 1][left + 2] = 1
                grid[top + 2][left + 1] = 1
    return grid


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", default="data/generated")
    parser.add_argument("--seed", type=int, default=20261103)
    args = parser.parse_args()

    output = Path(args.output_dir)
    rng = random.Random(args.seed)
    records: list[tuple[str, str, str]] = []

    def emit(category: str, name: str, grid: list[list[int]]) -> None:
        relative = Path(category) / f"{name}.in"
        write_case(output / relative, grid)
        records.append((category, name, str(relative).replace("\\", "/")))

    sizes = [(10, 10), (25, 25), (50, 50)]
    for size_index, (rows, cols) in enumerate(sizes):
        for percent in (10, 25, 50):
            category = f"random_sparse_p{percent}"
            emit(category, f"n{rows}_m{cols}",
                 random_degrees(rows, cols, percent / 100.0, rng))

        emit("random_dense", f"n{rows}_m{cols}",
             random_degrees(rows, cols, 0.85, rng))
        # 50x50 的高密度随机 planted 实例可能变成近乎对抗性的完整搜索负载。
        # 最大规模使用仍然稠密但可复现的 0.40，避免基准只测一个随机硬核。
        planted_density = 0.40 if rows >= 50 else 0.55
        emit("planted_feasible", f"n{rows}_m{cols}",
             planted_feasible(rows, cols, planted_density, rng))
        emit("fragmented", f"n{rows}_m{cols}_s{size_index}",
             planted_feasible(rows, cols, 0.16, rng))
        emit("infeasible_odd", f"n{rows}_m{cols}",
             odd_total_case(rows, cols, rng))

    emit("dense_all_positive", "n50_m50", all_horizontal(50, 50))
    emit("conflict_heavy", "n50_m50", conflict_trap(50, 50))
    emit("infeasible_boundary", "single_degree4", [[4, 0], [0, 0]])

    output.mkdir(parents=True, exist_ok=True)
    manifest = output / "manifest.csv"
    with manifest.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["category", "case", "input"])
        writer.writerows(records)


if __name__ == "__main__":
    main()
