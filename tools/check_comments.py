#!/usr/bin/env python3
"""Check submitted C++ comments for language and density requirements."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


HAN_RE = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]")


@dataclass
class SourceStats:
    path: Path
    total_lines: int = 0
    comment_lines: int = 0
    chinese_comment_lines: int = 0
    chinese_source_lines: int = 0
    unclosed_block_comment: bool = False

    def add(self, other: "SourceStats") -> None:
        self.total_lines += other.total_lines
        self.comment_lines += other.comment_lines
        self.chinese_comment_lines += other.chinese_comment_lines
        self.chinese_source_lines += other.chinese_source_lines
        self.unclosed_block_comment |= other.unclosed_block_comment

    @property
    def ratio(self) -> float:
        if self.total_lines == 0:
            return 0.0
        return 100.0 * self.comment_lines / self.total_lines


def scan_source(path: Path) -> SourceStats:
    """Count useful comment lines while ignoring comment markers inside strings."""
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    stats = SourceStats(
        path=path,
        total_lines=sum(bool(line.strip()) for line in lines),
        chinese_source_lines=sum(bool(HAN_RE.search(line)) for line in lines),
    )

    state = "code"
    comment_text: list[str] = []
    line_has_comment = False
    line_has_chinese_comment = False

    def finish_line() -> None:
        nonlocal line_has_comment
        nonlocal line_has_chinese_comment
        if line_has_comment:
            stats.comment_lines += 1
        if line_has_chinese_comment:
            stats.chinese_comment_lines += 1
        line_has_comment = False
        line_has_chinese_comment = False
        comment_text.clear()

    index = 0
    while index < len(text):
        ch = text[index]
        next_ch = text[index + 1] if index + 1 < len(text) else ""

        if ch == "\n":
            if state == "line_comment":
                state = "code"
            finish_line()
            index += 1
            continue

        if state == "line_comment":
            comment_text.append(ch)
            line_has_comment = line_has_comment or not ch.isspace()
            line_has_chinese_comment |= bool(HAN_RE.match(ch))
            index += 1
            continue

        if state == "block_comment":
            if ch == "*" and next_ch == "/":
                state = "code"
                index += 2
                continue
            comment_text.append(ch)
            line_has_comment = line_has_comment or not ch.isspace()
            line_has_chinese_comment |= bool(HAN_RE.match(ch))
            index += 1
            continue

        if state in {"string", "char"}:
            quote = '"' if state == "string" else "'"
            if ch == "\\" and next_ch:
                index += 2
                continue
            if ch == quote:
                state = "code"
            index += 1
            continue

        if ch == "/" and next_ch == "/":
            state = "line_comment"
            index += 2
            continue
        if ch == "/" and next_ch == "*":
            state = "block_comment"
            index += 2
            continue
        if ch == '"':
            state = "string"
            index += 1
            continue
        if ch == "'":
            state = "char"
            index += 1
            continue
        index += 1

    if state == "block_comment":
        stats.unclosed_block_comment = True
    if text and not text.endswith("\n"):
        finish_line()
    return stats


def find_sources(root: Path, scope: str) -> list[Path]:
    base = root / scope
    if not base.is_dir():
        return []
    return sorted(
        path for path in base.rglob("*")
        if path.is_file() and path.suffix in {".cpp", ".hpp"}
    )


def aggregate(paths: list[Path]) -> SourceStats:
    stats = SourceStats(path=Path("."))
    for path in paths:
        stats.add(scan_source(path))
    return stats


def print_scope(name: str, stats: SourceStats, minimum: float) -> bool:
    passed = stats.ratio + 1e-9 >= minimum
    print(
        f"{name}: {stats.comment_lines}/{stats.total_lines} "
        f"({stats.ratio:.2f}%), required >= {minimum:.2f}%"
    )
    return passed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--overall-min", type=float, default=30.0)
    parser.add_argument("--src-min", type=float, default=33.0)
    args = parser.parse_args()

    root = args.root.resolve()
    src_paths = find_sources(root, "src")
    test_paths = find_sources(root, "tests")
    all_paths = src_paths + test_paths
    if not src_paths or not test_paths:
        print("error: expected C++ files under both src/ and tests/", file=sys.stderr)
        return 2

    src_stats = aggregate(src_paths)
    all_stats = aggregate(all_paths)
    scoped_stats = [scan_source(path) for path in all_paths]

    problems: list[str] = []
    for stats in scoped_stats:
        relative = stats.path.relative_to(root)
        if stats.chinese_comment_lines:
            problems.append(
                f"{relative}: {stats.chinese_comment_lines} line(s) contain "
                "Chinese comment text"
            )
        if stats.chinese_source_lines:
            problems.append(
                f"{relative}: {stats.chinese_source_lines} line(s) contain "
                "Chinese source text"
            )
        if stats.unclosed_block_comment:
            problems.append(f"{relative}: unterminated block comment")

    ratio_ok = print_scope("src", src_stats, args.src_min)
    ratio_ok &= print_scope("all C++ source", all_stats, args.overall_min)
    print(
        f"Chinese source lines: {all_stats.chinese_source_lines}; "
        f"Chinese comment lines: {all_stats.chinese_comment_lines}"
    )

    for problem in problems:
        print(f"error: {problem}", file=sys.stderr)
    if problems or not ratio_ok:
        return 1
    print("comment quality gate passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
