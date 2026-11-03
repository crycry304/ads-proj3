#include "io.hpp"
#include "model.hpp"
#include "solver.hpp"
#include "validator.hpp"

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Tests link directly against the production modules, so a failure here is a
// failure in the submitted implementation rather than a parallel test model.
//
// Coverage has four layers: input parsing, static candidate construction,
// output serialization/validation, and geometric edge cases.
//
// Feasibility is cross-checked by two independent methods: BruteForceExists
// enumerates candidate-edge subsets on small instances, while Solver uses
// complete backtracking with propagation and MRV. They must agree.
//
// Validation checks constraints rather than one fixed answer because multiple
// layouts may be valid. Random tests use fixed seeds so failures reproduce.
//
// The brute-force oracle is deliberately written in a different style from the
// solver: it selects a complete subset mask, checks conflicts among that subset,
// and compares all degrees afterward. The solver instead grows a partial state
// and propagates local implications. Agreement therefore tests more than two
// implementations sharing the same control flow.
//
// For a model with E candidates, the oracle tries 2^E masks. It is restricted to
// E <= 20 so the exhaustive tests remain practical. The solver has no such test
// cutoff; the oracle is only a source of ground truth for small instances.

// Accumulate failures to report multiple issues in one run.
int failures = 0;

// Report the purpose of a failed assertion before continuing.
void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// Parse a text fixture and construct its static model.
recover::Model BuildFromText(const std::string& text) {
    std::istringstream input(text);
    recover::Grid grid;
    std::string error;
    Expect(recover::ReadSingleCase(&input, &grid, &error), error);

    // Parsing and model construction are independently checked stages.
    recover::Model model;
    Expect(recover::BuildModel(grid, &model, &error), error);
    return model;
}

// Validate a candidate answer through the production validator.
recover::ValidationResult ValidateText(const recover::Model& model,
                                       const std::string& text) {
    std::istringstream output(text);
    return recover::ValidateOutput(model, &output);
}

// Enumerate candidate-edge subsets without using any Solver decision order.
// This provides an independent feasibility oracle for small models.
bool BruteForceExists(const recover::Model& model) {
    const int edgeCount = model.CandidateEdgeCount();
    if (edgeCount > 20) {
        return false;
    }

    const std::uint64_t limit = std::uint64_t{1} << edgeCount;
    const int targetEdges = model.totalRequiredDegree / 2;
    // The target-edge count is a quick rejection criterion: every selected edge
    // contributes two degree units. Degree and conflict checks are still needed
    // because the count alone is not sufficient.

    for (std::uint64_t mask = 0; mask < limit; ++mask) {
        // A valid layout must select exactly half of the total degree.
        if (static_cast<int>(std::bitset<64>(mask).count()) != targetEdges) {
            continue;
        }

        std::vector<int> degree(model.vertexCount, 0);
        bool valid = true;
        for (int edgeId = 0; edgeId < edgeCount && valid; ++edgeId) {
            if ((mask & (std::uint64_t{1} << edgeId)) == 0) {
                continue;
            }

            const recover::CandidateEdge& edge = model.edges[edgeId];
            ++degree[edge.u];
            ++degree[edge.v];
            if (degree[edge.u] > model.requiredDegree[edge.u] ||
                degree[edge.v] > model.requiredDegree[edge.v]) {
                valid = false;
                break;
            }

            for (int other : model.conflictEdges[edgeId]) {
                if ((mask & (std::uint64_t{1} << other)) != 0) {
                    valid = false;
                    break;
                }
            }
        }

        if (!valid) {
            continue;
        }
        // Conflict-free selection still needs exact degree checks everywhere.
        for (int v = 0; v < model.vertexCount; ++v) {
            if (degree[v] != model.requiredDegree[v]) {
                valid = false;
                break;
            }
        }
        if (valid) {
            // Feasibility requires only one witness.
            return true;
        }
    }
    return false;
}

void TestSolverAndValidation(const std::vector<std::vector<int>>& grid,
                             bool expectedFound) {
    recover::Model model;
    std::string error;
    Expect(recover::BuildModel(grid, &model, &error), error);

    recover::Solver solver(model);
    const recover::SolveResult result = solver.Solve();
    // Compare the solver conclusion with the fixture expectation.
    Expect(result.found == expectedFound, "solver solvability mismatch");
    if (!result.found) {
        // No constructive answer exists to validate in this case.
        return;
    }

    // Serialize a constructive answer and validate it independently.
    std::ostringstream output;
    recover::PrintDirections(model, result.directions, &output);
    Expect(ValidateText(model, output.str()).status ==
               recover::ValidationStatus::kValid,
           "solver output must pass independent validation");
}

// Compare Solver with the brute-force oracle, then validate every witness.
// context identifies the grid size and deterministic fixture on failure.
void CheckDifferentialCase(const std::vector<std::vector<int>>& grid,
                           const std::string& context) {
    // Build from the input before computing either conclusion.
    recover::Model model;
    std::string error;
    Expect(recover::BuildModel(grid, &model, &error),
           context + ": " + error);

    // The oracle must not depend on any solver trace.
    const bool expected = BruteForceExists(model);
    // If the oracle finds a witness, the solver must also find one; if the
    // solver fails, this assertion makes the disagreement immediately visible.
    recover::Solver solver(model);
    const recover::SolveResult result = solver.Solve();
    Expect(result.found == expected,
           context + ": solver disagrees with brute force");
    if (!result.found) {
        return;
    }

    std::ostringstream output;
    recover::PrintDirections(model, result.directions, &output);
    const recover::ValidationResult validation =
        ValidateText(model, output.str());
    Expect(validation.status == recover::ValidationStatus::kValid,
           context + ": " + validation.message);
}

// Exhaust every 2x2 and 2x3 degree matrix in the legal input space.
void TestExhaustiveSmallGrids() {
    // Each cell ranges over 0..4; 2x3 has only 5^6 = 15,625 instances.
    for (const auto& dimensions : std::vector<std::pair<int, int>>{
             {2, 2}, {2, 3}}) {
        const int rows = dimensions.first;
        const int cols = dimensions.second;
        // Encode the whole matrix as a base-5 counter.
        const int cells = rows * cols;
        // Compute the number of encoded matrices.
        // The integer is small enough for the fixed test set; this is an
        // enumeration counter, not a bound derived from arbitrary input.
        int total = 1;
        for (int i = 0; i < cells; ++i) {
            total *= 5;
        }

        for (int code = 0; code < total; ++code) {
            // Decode one cell at a time.
            int value = code;
            std::vector<std::vector<int>> grid(
                rows, std::vector<int>(cols, 0));
            // Each base-5 digit is one connector degree.
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    grid[r][c] = value % 5;
                    value /= 5;
                }
            }
            CheckDifferentialCase(
                grid, "exhaustive " + std::to_string(rows) + "x" +
                          std::to_string(cols) + " code " +
                          std::to_string(code));
            if (failures > 5) {
                // Stop after repeated failures to keep diagnostics readable.
                return;
            }
        }
    }
}

// The 3x3 and 3x4 instances have few enough candidates for subset enumeration.
void TestRandomThreeByThreeAndThreeByFour() {
    // A 3x4 grid has at most about 14 candidates, so 2^14 enumeration is cheap.
    std::mt19937 generator(20261103U);
    std::uniform_int_distribution<int> rowsDistribution(3, 3);
    std::uniform_int_distribution<int> colsDistribution(3, 4);
    std::bernoulli_distribution nonZero(0.45);
    std::uniform_int_distribution<int> degreeDistribution(1, 4);

    for (int test = 0; test < 3000; ++test) {
        const int rows = rowsDistribution(generator);
        const int cols = colsDistribution(generator);
        std::vector<std::vector<int>> grid(
            rows, std::vector<int>(cols, 0));
        // First choose connector presence, then an occupied degree.
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (nonZero(generator)) {
                    grid[r][c] = degreeDistribution(generator);
                }
            }
        }
        // Each iteration exercises a fresh model and both feasibility outcomes;
        // the fixed seed makes an unexpected disagreement reproducible.
        CheckDifferentialCase(grid, "random differential " +
                                         std::to_string(test));
        if (failures > 5) {
            return;
        }
    }
}

// Plant a conflict-free edge set first, then derive connector degrees from it.
// These instances are feasible by construction, so the solver must never report
// No Solution for one of them.
void TestPlantedFeasibleLayouts() {
    // The hidden edge set is a witness that the generated input is feasible.
    // Unlike arbitrary random grids, this family isolates the specific error
    // mode of rejecting an instance merely because search pruning is too strong.
    std::mt19937 generator(20261104U);
    std::uniform_int_distribution<int> sizeDistribution(3, 6);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    for (int test = 0; test < 200; ++test) {
        // Vary dimensions and connector density deterministically.
        const int rows = sizeDistribution(generator);
        const int cols = sizeDistribution(generator);
        const double density = unit(generator);

        std::vector<std::vector<int>> mask(
            rows, std::vector<int>(cols, 0));
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                mask[r][c] = unit(generator) < density ? 1 : 0;
            }
        }

        // Build with placeholder degrees to obtain candidate geometry.
        recover::Model base;
        std::string error;
        Expect(recover::BuildModel(mask, &base, &error), error);
        if (base.CandidateEdgeCount() == 0) {
            // No candidates means there is no hidden fence to plant.
            continue;
        }

        // Random order prevents the planted set from following candidate ids.
        std::vector<int> order(base.CandidateEdgeCount());
        for (int i = 0; i < base.CandidateEdgeCount(); ++i) {
            order[i] = i;
        }
        std::shuffle(order.begin(), order.end(), generator);
        // Starting from candidate ids alone would bias the planted layout
        // toward the solver's construction order, so shuffle the selection order.

        std::vector<bool> selected(base.CandidateEdgeCount(), false);
        std::vector<int> vertexDegree(base.vertexCount, 0);
        // Each planted edge contributes one degree at both endpoints.
        for (int edgeId : order) {
            // Include roughly 55 percent unless a crossing makes it illegal.
            if (unit(generator) > 0.55) {
                continue;
            }
            bool conflicts = false;
            for (int other : base.conflictEdges[edgeId]) {
                if (selected[other]) {
                    conflicts = true;
                    break;
                }
            }
            if (conflicts) {
                // Never place two conflicting edges in the hidden layout.
                continue;
            }
            selected[edgeId] = true;
            const recover::CandidateEdge& edge = base.edges[edgeId];
            ++vertexDegree[edge.u];
            ++vertexDegree[edge.v];
        }

        // Materialize the planted degrees into the input matrix.
        std::vector<std::vector<int>> grid(
            rows, std::vector<int>(cols, 0));
        bool any = false;
        for (int v = 0; v < base.vertexCount; ++v) {
            grid[base.vertexRow[v]][base.vertexCol[v]] = vertexDegree[v];
            any = any || vertexDegree[v] > 0;
        }
        if (any) {
            // At least one edge was planted, so a witness exists.
            TestSolverAndValidation(grid, true);
        }
    }
}

// Cover geometric and counting boundaries that are easy to misinterpret.
// The second argument states whether a layout must exist.
void TestRequiredEdgeCases() {
    // A lone connector has no partner.
    TestSolverAndValidation({{1, 0}, {0, 0}}, false);

    // Adjacent horizontal connectors require the segment between them.
    TestSolverAndValidation({{1, 1}, {0, 0}}, true);

    // The same applies to adjacent vertical connectors.
    TestSolverAndValidation({{1, 0}, {1, 0}}, true);

    // One long horizontal segment spans two empty cells.
    TestSolverAndValidation({{1, 0, 0, 1}, {0, 0, 0, 0}}, true);

    // A four-way connector joins four one-degree arms.
    // This distinguishes a turnaround at a connector from an illegal crossing
    // through an empty cell, which is covered separately below.
    TestSolverAndValidation(
        {{0, 1, 0}, {1, 4, 1}, {0, 1, 0}}, true);

    // A three-way connector uses its up, left, and right candidates.
    TestSolverAndValidation(
        {{0, 1, 0}, {1, 3, 1}, {0, 0, 0}}, true);

    // Separate valid components are allowed; the problem does not require one
    // connected fence system.
    // The two small components exercise the component-local parity check rather
    // than a global total alone.
    TestSolverAndValidation(
        {{1, 1, 0, 1, 1}, {0, 0, 0, 0, 0}}, true);

    // A corner connector has only two directions but demands four.
    TestSolverAndValidation({{4, 0}, {0, 0}}, false);

    // Odd total demand cannot be formed by degree contributions of two.
    TestSolverAndValidation({{1, 1, 0}, {1, 0, 0}}, false);
}

// Keep all three official samples as fixed regressions. Solutions need not
// match one stored string; every witness is checked for validity instead.
void TestSolverSamples() {
    // Sample 1 is the ordinary feasible case.
    TestSolverAndValidation(
        {
            {0, 0, 2, 0, 0, 2},
            {1, 3, 3, 1, 0, 0},
            {2, 2, 1, 0, 0, 2},
            {0, 0, 0, 0, 0, 1},
            {2, 0, 2, 0, 0, 2},
        },
        true);

    // Sample 2 has odd total demand and must fail static validation.
    TestSolverAndValidation(
        {
            {0, 0, 2, 0, 0, 2},
            {1, 3, 3, 1, 0, 0},
            {2, 2, 1, 0, 0, 1},
            {0, 0, 0, 0, 0, 1},
            {2, 0, 2, 0, 0, 2},
        },
        false);

    // Sample 3 includes long segments and one empty-cell conflict.
    // The published answer contains several rows, so this also checks that the
    // serializer can represent a nontrivial row-major output order.
    TestSolverAndValidation(
        {
            {1, 1, 2, 0, 1},
            {1, 0, 3, 0, 2},
            {1, 2, 1, 0, 1},
        },
        true);
}

// Fixed-seed differential tests cover 2x2 through 3x4 grids.
void TestSolverAgainstBruteForce() {
    // A fixed seed makes every reported failure reproducible.
    std::mt19937 generator(20261102U);
    std::uniform_int_distribution<int> rows(2, 3);
    std::uniform_int_distribution<int> cols(2, 4);
    std::bernoulli_distribution nonZero(0.45);
    std::uniform_int_distribution<int> degree(1, 4);

    for (int test = 0; test < 1000; ++test) {
        const int n = rows(generator);
        const int m = cols(generator);
        std::vector<std::vector<int>> grid(n, std::vector<int>(m, 0));
        for (int r = 0; r < n; ++r) {
            for (int c = 0; c < m; ++c) {
                if (nonZero(generator)) {
                    grid[r][c] = degree(generator);
                }
            }
        }

        recover::Model model;
        std::string error;
        Expect(recover::BuildModel(grid, &model, &error), error);

        const bool expected = BruteForceExists(model);
        // The production solver and oracle must agree on feasibility.
        recover::Solver solver(model);
        const recover::SolveResult result = solver.Solve();
        // This covers both feasible and infeasible generated instances.
        Expect(result.found == expected,
               "random model disagrees with brute-force result");
        if (result.found) {
            // Random differential cases must also survive the independent
            // validator; a matching Boolean is insufficient if the constructed
            // directions are malformed or omit a candidate endpoint.
            std::ostringstream output;
            recover::PrintDirections(model, result.directions, &output);
            Expect(ValidateText(model, output.str()).status ==
                       recover::ValidationStatus::kValid,
                   "random solver witness must validate");
        }
        if (failures > 5) {
            // Limit repeated failures to keep the output readable.
            return;
        }
    }
}

// Require both feasibility and evidence that the queue actually forced an edge.
void TestForcedPropagation() {
    // The top connectors demand 1, 2, 1. Selecting either horizontal edge leaves
    // the middle connector needing its one remaining candidate.
    // This fixture is small enough that the queue behavior is easy to inspect
    // while still requiring two chained forced selections after the first edge.
    recover::Model model = BuildFromText(
        "2 3\n"
        "1 2 1\n"
        "0 0 0\n");
    recover::Solver solver(model);
    const recover::SolveResult result = solver.Solve();

    Expect(result.found, "forced-selection fixture must be solvable");
    Expect(result.stats.propagationEvents > 0,
           "propagation queue must process changed endpoints");
    Expect(result.stats.forcedSelections > 0,
           "fixture must trigger forced selection");
    // The assertion is intentionally statistic-based: a solver that merely
    // guesses the same answer without using the queue would fail this test.
}

// Check both infeasibility and the conflictRejections counter. The second
// assertion prevents a removed conflict rule from going unnoticed merely because
// another constraint still happens to make the fixture impossible.
void TestConflictPropagation() {
    // Four one-degree endpoints create perpendicular candidates that cross at
    // the center zero. Selecting one must reject the other through propagation.
    recover::Model model = BuildFromText(
        "3 3\n"
        "0 1 0\n"
        "1 0 1\n"
        "0 1 0\n");
    recover::Solver solver(model);
    const recover::SolveResult result = solver.Solve();

    Expect(!result.found, "crossing fixture must be impossible");
    Expect(result.stats.conflictRejections > 0,
           "selected edge must reject its crossing edge");
    // This is a causal check, not a duplicate of the feasibility assertion.
    // The crossing edge must be removed by conflict propagation after the first
    // selection, before the remaining degree equations fail.
}

// Exercise valid input, out-of-range degree rejection, and trailing-token
// rejection. These tests separate malformed input from an infeasible instance.
void TestParser() {
    // Preserve the stated dimensions for a valid matrix.
    recover::Grid grid;
    std::string error;
    std::istringstream valid("2 3\n1 0 2\n0 3 0\n");
    Expect(recover::ReadSingleCase(&valid, &grid, &error), "valid input");
    Expect(grid.size() == 2 && grid[0].size() == 3, "grid dimensions");

    // Degree 5 exceeds the four incident directions.
    std::istringstream badDegree("2 2\n1 2\n3 5\n");
    Expect(!recover::ReadSingleCase(&badDegree, &grid, &error),
           "degree 5 must be rejected");

    // A second case or stray integer is not part of the required format.
    std::istringstream extraToken("2 2\n1 0\n0 0\n7\n");
    Expect(!recover::ReadSingleCase(&extraToken, &grid, &error),
           "extra input token must be rejected");
}

// Check static construction without invoking Solver. Sample 3 tests long
// segments and conflict geometry; Sample 2 tests early parity infeasibility.
// Exact counts catch missing or duplicated candidates and conflicts.
void TestCandidateModel() {
    // Sample 3 has a horizontal candidate spanning an empty cell.
    const recover::Model sample3 = BuildFromText(
        "3 5\n"
        "1 1 2 0 1\n"
        "1 0 3 0 2\n"
        "1 2 1 0 1\n");
    // Eleven connectors and fifteen row/column candidates expose one crossing.
    Expect(sample3.vertexCount == 11, "sample 3 connector count");
    // Verify the total candidate count.
    // The exact count catches both an omitted maximal run and a duplicate edge
    // created while scanning the grid.
    Expect(sample3.CandidateEdgeCount() == 15, "sample 3 edge count");
    Expect(sample3.conflictPairCount == 1, "sample 3 conflict count");
    // Static feasibility is necessary but does not itself certify a solution.
    Expect(sample3.structurallyPossible, "sample 3 static feasibility");

    // Sample 2 has odd total demand and fails before search.
    const recover::Model sample2 = BuildFromText(
        "5 6\n"
        "0 0 2 0 0 2\n"
        "1 3 3 1 0 0\n"
        "2 2 1 0 0 1\n"
        "0 0 0 0 0 1\n"
        "2 0 2 0 0 2\n");
    // Every fence contributes two degree units, so 25 cannot be partitioned.
    Expect(sample2.totalRequiredDegree == 25,
           "sample 2 total requested degree");
    Expect(!sample2.structurallyPossible,
           "odd total degree must be rejected statically");
}

// Cover parsing, validation, serialization round trips, and wrong-degree
// rejection. A No Solution fixture checks recognition of the text marker only;
// the marker is not treated as proof of global infeasibility.
void TestOutputValidation() {
    // Validate the official Sample 1 witness.
    const recover::Model sample1 = BuildFromText(
        "5 6\n"
        "0 0 2 0 0 2\n"
        "1 3 3 1 0 0\n"
        "2 2 1 0 0 2\n"
        "0 0 0 0 0 1\n"
        "2 0 2 0 0 2\n");
    // Each of the 14 lines stores row, column, up, down, left, and right.
    // The fixture is copied as an expected source artifact rather than produced
    // by the solver under test, so a serializer regression cannot silently
    // change both sides of the comparison.
    const recover::ValidationResult sample1Result = ValidateText(
        sample1,
        "1 3 0 1 0 1\n"
        "1 6 0 1 1 0\n"
        "2 1 0 0 0 1\n"
        "2 2 0 1 1 1\n"
        "2 3 1 0 1 1\n"
        "2 4 0 0 1 0\n"
        "3 1 0 1 0 1\n"
        "3 2 1 0 1 0\n"
        "3 3 0 0 0 1\n"
        "3 6 1 0 1 0\n"
        "4 6 0 1 0 0\n"
        "5 1 1 0 0 1\n"
        "5 3 0 0 1 1\n"
        "5 6 1 0 1 0\n");
    Expect(sample1Result.status == recover::ValidationStatus::kValid,
           "sample 1 output must validate");

    // Reparse the validator's direction arrays to cover coordinate conversion
    // and row-major serialization in the opposite direction.
    std::ostringstream roundTrip;
    recover::PrintDirections(sample1, sample1Result.directions, &roundTrip);
    Expect(ValidateText(sample1, roundTrip.str()).status ==
               recover::ValidationStatus::kValid,
           "print/validate round trip");

    // Sample 3 admits multiple answers; validate the published witness.
    const recover::Model sample3 = BuildFromText(
        "3 5\n"
        "1 1 2 0 1\n"
        "1 0 3 0 2\n"
        "1 2 1 0 1\n");
    const recover::ValidationResult sample3Result = ValidateText(
        sample3,
        "1 1 0 0 0 1\n"
        "1 2 0 0 1 0\n"
        "1 3 0 1 0 1\n"
        "1 5 0 0 1 0\n"
        "2 1 0 0 0 1\n"
        "2 3 1 0 1 1\n"
        "2 5 0 1 1 0\n"
        "3 1 0 0 0 1\n"
        "3 2 0 0 1 1\n"
        "3 3 0 0 1 0\n"
        "3 5 1 0 0 0\n");
    Expect(sample3Result.status == recover::ValidationStatus::kValid,
           "sample 3 output must validate");

    // The output validator can recognize this claim but cannot prove it.
    Expect(ValidateText(sample3, "No Solution\n").status ==
               recover::ValidationStatus::kNoSolution,
           "No Solution marker");
    // Change the degree of the final connector from one to zero.
    std::string wrongDegree =
        "1 1 0 0 0 1\n"
        "1 2 0 0 1 0\n"
        "1 3 0 1 0 1\n"
        "1 5 0 0 1 0\n"
        "2 1 0 0 0 1\n"
        "2 3 1 0 1 1\n"
        "2 5 0 1 1 0\n"
        "3 1 0 0 0 1\n"
        "3 2 0 0 1 1\n"
        "3 3 0 0 1 0\n"
        "3 5 0 0 0 0\n";
    Expect(ValidateText(sample3, wrongDegree).status ==
               recover::ValidationStatus::kInvalid,
           "wrong degree must fail");
}

// This witness has correct endpoint pairing and degree at all four connectors.
// Its only error is that the two long segments cross at an empty center cell.
void TestIllegalCrossing() {
    // The four arm endpoints create one conflict through the center zero.
    const recover::Model crossing = BuildFromText(
        "3 3\n"
        "0 1 0\n"
        "1 0 1\n"
        "0 1 0\n");
    Expect(crossing.conflictPairCount == 1,
           "crossing model must contain one conflict");
    // The model count also guards the distinction between a geometric crossing
    // and two independent pairs of endpoints with otherwise identical degrees.

    // Every endpoint has degree one, but both segments are selected together.
    const std::string layout =
        "1 2 0 1 0 0\n"
        "2 1 0 0 0 1\n"
        "2 3 0 0 1 0\n"
        "3 2 1 0 0 0\n";
    // Reciprocal directions are consistent in both candidates.
    Expect(ValidateText(crossing, layout).status ==
               recover::ValidationStatus::kInvalid,
           "fences crossing at an empty point must fail");
    // A validator that only sums endpoint flags would incorrectly accept this
    // layout, so the failure specifically exercises the conflict graph.
}

// An all-zero input has a valid empty representation, not a malformed answer.
void TestEmptyLayout() {
    // No connectors means no output lines are required.
    const recover::Model empty = BuildFromText(
        "2 2\n"
        "0 0\n"
        "0 0\n");
    const recover::ValidationResult result = ValidateText(empty, "");
    // The validator must accept this exact empty textual representation.
    Expect(result.status == recover::ValidationStatus::kValid,
           "all-zero grid accepts an empty layout");
    // This is distinct from the No Solution marker: the input has a valid empty
    // realization, and no fabricated "No Solution" text is required.
}

}  // namespace

int main() {
    // Run independent groups in a deterministic order.
    TestParser();
    TestCandidateModel();
    TestOutputValidation();
    TestIllegalCrossing();
    TestEmptyLayout();
    TestSolverSamples();
    TestSolverAgainstBruteForce();
    TestForcedPropagation();
    TestConflictPropagation();
    TestExhaustiveSmallGrids();
    TestRandomThreeByThreeAndThreeByFour();
    TestPlantedFeasibleLayouts();
    TestRequiredEdgeCases();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All model and validation tests passed\n";
    return 0;
}
