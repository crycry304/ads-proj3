# Recover the Design

This repository contains our solution and experiments for the Advanced Data Structures and Algorithm Analysis Project 3 problem, **Recover the Design**.

## Problem overview

The input describes a rectangular grid of connector positions. Each positive value `k` requires exactly `k` fence segments to meet at that connector, while `0` means that no connector exists. Every fence is straight and either horizontal or vertical. A fence may pass through empty positions, but horizontal and vertical fences may cross only when the crossing position contains a connector.

The program must reconstruct one valid layout or report `No Solution`.

## Modeling

For every row, consecutive non-zero connectors define possible horizontal fence segments. The same construction is used per column for vertical segments. A valid solution selects a subset of these candidate segments such that:

- every connector has exactly the requested number of incident segments;
- a selected segment connects both of its endpoints;
- selected horizontal and vertical segments do not cross at an empty grid position.

This is an exact-degree constraint problem with pairwise crossing conflicts. It is not sufficient to apply a plain bipartite matching reduction because candidate segments may span empty cells and the candidate graph is not guaranteed to be bipartite.

## Algorithm

The exact solver will use:

1. static feasibility checks and candidate-segment construction;
2. a precomputed conflict graph for crossings at empty positions;
3. depth-first search with explicit selected/rejected/unknown states;
4. event-driven constraint propagation with rollback;
5. minimum-remaining-values branching over at most six local direction combinations per connector;
6. an independent output validator and small-instance brute-force differential tests.

The algorithm stops after finding the first valid layout, since the problem accepts any solution.

## Repository status

The exact solver is implemented with event-driven propagation, conflict rejection,
forced decisions, rollback, and MRV branching. The correctness suite, reproducible
benchmark corpus, and illustrated anonymous report are complete.

## Repository layout

The repository has the following structure:

```text
ads-proj3/
├── .gitignore
├── README.md
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── model.hpp
│   ├── model.cpp
│   ├── solver.hpp
│   ├── solver.cpp
│   ├── validator.hpp
│   └── validator.cpp
├── tests/
│   ├── samples/
│   ├── benchmark_runner.cpp
│   └── test_runner.cpp
├── tools/
│   ├── check_comments.py
│   ├── generate_cases.py
│   ├── make_report_figures.py
│   ├── prepare_report_data.py
│   └── run_benchmarks.py
├── data/
│   └── experiments/
└── reports/
    ├── report.tex
    ├── report.pdf
    └── figures/
```

Local-only planning files such as `PROBLEM_ANALYSIS.md`, `notes.md`, and `plan.md` are intentionally excluded from Git.

## Planned submission package

The course submission ZIP will be separate from the repository checkout:

```text
Teacher_2024G##_P3.zip
├── documents/
│   └── Teacher_2024G##_P3.pdf
└── code/
    ├── README.txt
    ├── CMakeLists.txt
    ├── src/
    └── tests/
```

The exact teacher identifier, group number, and current-year naming rule must be checked against the latest course instructions before creating the final ZIP.

## Test strategy

The three samples cover:

- a feasible design;
- an immediate impossible case whose total requested degree is odd;
- a non-unique case with a crossing conflict.

Implementation testing includes exhaustive small-grid differential tests,
randomized feasible and infeasible instances, boundary cases, and maximum-size
stress tests.

## Validation status

- Debug and Release builds pass without compiler warnings.
- The CTest suite covers the official samples, all 2×2 and 2×3 degree
  matrices, 4,000 randomized differential cases, planted feasible layouts,
  propagation statistics, conflicts, and required geometric edge cases.
- Every constructed answer is checked again by the independent validator.
- The 24-case Release benchmark corpus completes within the 30-second safety
  timeout; its maximum observed median is about 0.333 seconds.
- The problem statement publishes no time limit, so performance is reported as
  an empirical comparison rather than an artificially fixed pass threshold.
- All submitted C++ comments are English, and no Chinese source text remains.
- The comment quality gate passes at 33% for `src/` and 30% for all C++ source.

## Build and usage

Build and run from the repository root:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
Get-Content tests\samples\sample1.in | .\build\Release\recover.exe
```

The submitted program reads one test case from standard input and writes either
the connector states or `No Solution` to standard output.

Validate an existing answer:

```powershell
.\build\Release\recover.exe --validate tests\samples\sample1.in tests\samples\sample1.out
```

Generate deterministic validation and benchmark cases:

```powershell
python tools/generate_cases.py --output-dir data/generated
```

Run the internal benchmark executable:

```powershell
.\build\Release\recover_benchmark.exe --repeat 5 data\generated\...\*.in
```

Check the submitted comment language and density before committing:

```powershell
python tools\check_comments.py
```

Prepare report figure data and compile the anonymous report:

```powershell
python tools/prepare_report_data.py
Set-Location reports
xelatex -interaction=nonstopmode report.tex
xelatex -interaction=nonstopmode report.tex
```

The primary report source is `reports/report.tex`; `reports/main.tex` is kept as
a compatibility wrapper.
