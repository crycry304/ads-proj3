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

## Planned algorithm

The exact solver will use:

1. static feasibility checks and candidate-segment construction;
2. a precomputed conflict graph for crossings at empty positions;
3. depth-first search with explicit selected/rejected/unknown states;
4. event-driven constraint propagation with rollback;
5. minimum-remaining-values branching over at most six local direction combinations per connector;
6. an independent output validator and small-instance brute-force differential tests.

The algorithm stops after finding the first valid layout, since the problem accepts any solution.

## Repository status

Problem analysis and the implementation plan are complete. The production solver, test harness, benchmark results, and final report are still to be added.

## Test strategy

The three samples cover:

- a feasible design;
- an immediate impossible case whose total requested degree is odd;
- a non-unique case with a crossing conflict.

Implementation testing will additionally include exhaustive small-grid differential tests, randomized feasible and infeasible instances, boundary cases, and maximum-size stress tests.

## Build and usage

The final build and execution commands will be added with the implementation. The submitted program is planned to read one test case from standard input and write either the connector states or `No Solution` to standard output.
