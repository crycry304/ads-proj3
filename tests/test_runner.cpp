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
//
// The executable also acts as a small test dispatcher. Each name registered in
// kTestGroups corresponds to one CTest entry, which lets a failure identify the
// affected subsystem immediately. Running the executable without arguments is
// still supported for local debugging and preserves the original all-tests path.
//
// Test categories have deliberately different emphases:
// - parser tests validate text grammar and non-mutation on failure;
// - model tests check graph construction, reciprocal indices, and static proofs;
// - validator tests inspect independent output reconstruction and every rejection;
// - samples and geometry tests cover the published and hand-derived boundaries;
// - differential tests compare an unrelated exhaustive oracle with the solver;
// - planted tests start from a hidden witness to catch over-aggressive pruning;
// - propagation tests assert statistics that prove a pruning path was exercised.
//
// Correctness assertions favor exact error messages for validator branches. The
// messages are small, stable parts of the diagnostic interface: changing one
// usually means a branch was moved or removed. Parser assertions likewise check
// messages, while solver assertions compare semantic outcomes or statistics.
//
// The test harness intentionally does not compare a solver answer with one
// stored answer string. Multiple legal layouts can exist, and the validator is
// the authority for degree, endpoint, ordering, and crossing constraints. The
// stored sample outputs are used by CLI black-box tests instead.
//
// A failed parser call must leave the caller's previous Grid untouched. This is
// stronger than merely returning false: partially published data would make a
// caller continue with a matrix that does not correspond to the input text.
//
// Exhaustive tests enumerate inputs, while random tests choose a deterministic
// sample from a larger space. Fixed seeds are part of the test contract because
// they make every failure reproducible and keep benchmark comparisons stable.
//
// The brute-force and solver paths are deliberately not factored together. Any
// shared helper that accidentally encoded the same pruning rule would weaken the
// differential oracle, so small duplication is preferable to false confidence.
//
// Statistics are not treated as cosmetic instrumentation. A forced-selection
// fixture must increment the corresponding counter, and a crossing fixture must
// increment conflict rejections; otherwise a solver that guessed the expected
// answer without exercising the advertised mechanism could still pass.
//
// Coverage is also checked qualitatively, not only by test count. Randomized
// differential groups require both feasible and infeasible outcomes, at least
// one rollback, and at least one propagation event. These assertions catch a
// generator or filter that accidentally reduces thousands of cases to a narrow
// class of trivial inputs.
//
// Tests that can fail early use a small failure cap. The cap keeps logs readable
// when one modeling bug causes thousands of equivalent disagreements, while a
// single failure still makes the process exit with a non-zero status.
// A zero-failure run therefore means every case in the selected group completed.

// Accumulate failures to report multiple issues in one run.
int failures = 0;

// Report the purpose of a failed assertion before continuing.
void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// Aggregate evidence that randomized differential tests reached every major
// solver outcome rather than exercising only trivial infeasible inputs.
struct DifferentialCoverage {
    // Count both possible solver conclusions.
    int feasible = 0;
    int infeasible = 0;

    // Count work that proves non-leaf search and queue handling occurred.
    std::uint64_t backtracks = 0;
    std::uint64_t propagationEvents = 0;
};

void RecordDifferentialCoverage(const recover::SolveResult& result,
                                DifferentialCoverage* coverage) {
    if (coverage == nullptr) {
        return;
    }
    if (result.found) {
        ++coverage->feasible;
    } else {
        ++coverage->infeasible;
    }
    coverage->backtracks += result.stats.backtracks;
    coverage->propagationEvents += result.stats.propagationEvents;
}

void ExpectDifferentialCoverage(const DifferentialCoverage& coverage,
                                const std::string& context) {
    // These assertions protect against a generator that accidentally produces
    // only trivial zero grids or only immediately impossible instances.
    Expect(coverage.feasible > 0,
           context + " must contain at least one feasible case");
    Expect(coverage.infeasible > 0,
           context + " must contain at least one infeasible case");
    Expect(coverage.backtracks > 0,
           context + " must exercise rollback after failed branches");
    Expect(coverage.propagationEvents > 0,
           context + " must exercise queued propagation");
}

// Parse a text fixture and construct its static model.
recover::Model BuildFromText(const std::string& text) {
    std::istringstream input(text);
    recover::Grid grid;
    std::string error;
    Expect(recover::ReadSingleCase(&input, &grid, &error), error);

    // Parsing and model construction are independently checked stages. A
    // failure in either stage increments the shared assertion counter.
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

void ExpectValidation(const recover::Model& model, const std::string& text,
                      recover::ValidationStatus expectedStatus,
                      const std::string& expectedMessage,
                      const std::string& context) {
    // Check both the machine-readable status and the exact diagnostic. Status
    // alone could hide a different rejection happening earlier than intended.
    const recover::ValidationResult result = ValidateText(model, text);
    Expect(result.status == expectedStatus, context + ": unexpected status");
    Expect(result.message == expectedMessage,
           context + ": expected message '" + expectedMessage + "', got '" +
               result.message + "'");
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
                           const std::string& context,
                           DifferentialCoverage* coverage = nullptr) {
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
    RecordDifferentialCoverage(result, coverage);
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
    DifferentialCoverage coverage;

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
                                         std::to_string(test),
                              &coverage);
        if (failures > 5) {
            return;
        }
    }
    ExpectDifferentialCoverage(coverage, "3x3/3x4 random differential group");
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
    DifferentialCoverage coverage;

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
        RecordDifferentialCoverage(result, &coverage);
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
    ExpectDifferentialCoverage(coverage, "2x2/3x4 random differential group");
}

// Require both feasibility and evidence that the queue actually forced an edge.
void TestForcedPropagation() {
    // The first row demands 1, 2, 1, 2 and has two one-degree connectors below
    // its final position. Selecting AB forces BC; the third connector then
    // reaches its target and forces rejection of CD, leaving the fourth
    // connector without enough remaining candidates.
    recover::Model model = BuildFromText(
        "3 4\n"
        "1 2 1 2\n"
        "0 0 0 1\n"
        "0 0 0 1\n");
    recover::Solver solver(model);
    const recover::SolveResult result = solver.Solve();

    Expect(!result.found, "forced-propagation fixture must be infeasible");
    Expect(result.stats.propagationEvents > 0,
           "propagation queue must process changed endpoints");
    Expect(result.stats.forcedSelections > 0,
           "fixture must trigger forced selection");
    Expect(result.stats.forcedRejections > 0,
           "fixture must trigger forced rejection");
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

void ExpectReadFailure(const std::string& input, const std::string& message,
                       const std::string& context) {
    const recover::Grid original = {{7, 8}, {9, 10}};
    recover::Grid grid = original;
    std::string error;
    std::istringstream stream(input);
    Expect(!recover::ReadSingleCase(&stream, &grid, &error), context);
    Expect(error == message,
           context + ": expected '" + message + "', got '" + error + "'");
    Expect(grid == original,
           context + ": failed parsing must preserve the caller's grid");
}

// Cover the complete parser contract. Failed parsing is distinct from a
// well-formed but infeasible puzzle, and must not publish a partial matrix.
void TestParser() {
    recover::Grid grid;
    std::string error;
    std::istringstream valid("2 3\n1 0 2\n0 3 0\n");
    Expect(recover::ReadSingleCase(&valid, &grid, &error), "valid input");
    Expect(grid.size() == 2 && grid[0].size() == 3, "grid dimensions");

    // Missing dimensions and non-numeric dimensions fail before allocation.
    ExpectReadFailure("", "missing n or m", "empty input");
    ExpectReadFailure("2", "missing n or m", "missing m");
    ExpectReadFailure("x 3\n", "missing n or m", "non-numeric n");

    // The statement requires both dimensions to be at least two and at most
    // fifty. These four checks cover each side of that closed interval.
    ExpectReadFailure("1 3\n1 1 1\n", "n and m must satisfy 2 <= n,m <= 50",
                      "too few rows");
    ExpectReadFailure("3 1\n1\n1\n1\n", "n and m must satisfy 2 <= n,m <= 50",
                      "too few columns");
    ExpectReadFailure("51 2\n", "n and m must satisfy 2 <= n,m <= 50",
                      "too many rows");
    ExpectReadFailure("2 51\n", "n and m must satisfy 2 <= n,m <= 50",
                      "too many columns");

    // A connector has at most four physical directions, so both endpoints of
    // the legal degree range are meaningful edge cases.
    ExpectReadFailure("2 2\n1 2\n3 -1\n",
                      "every connector degree must be in [0, 4]",
                      "negative degree");
    ExpectReadFailure("2 2\n1 2\n3 5\n",
                      "every connector degree must be in [0, 4]",
                      "degree greater than four");

    // Short input and non-numeric cells are distinct lexical failures even
    // though both prevent construction of a complete matrix.
    ExpectReadFailure("2 3\n1 0 2\n0 3\n", "grid is shorter than n*m",
                      "short grid");
    ExpectReadFailure("2 2\n1 x\n0 0\n", "grid is shorter than n*m",
                      "non-numeric cell");

    // Exactly one case is accepted. Rejecting trailing data prevents a second
    // test case from being silently consumed as part of the first matrix.
    ExpectReadFailure("2 2\n1 0\n0 0\n7\n",
                      "extra token after the single test case",
                      "trailing token");
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

    int conflictEntries = 0;
    for (int edgeId = 0; edgeId < sample3.CandidateEdgeCount(); ++edgeId) {
        const recover::CandidateEdge& edge = sample3.edges[edgeId];
        Expect(sample3.directionEdge[edge.u][edge.directionAtU] == edgeId,
               "candidate direction index at u must be reciprocal");
        Expect(sample3.directionEdge[edge.v][edge.directionAtV] == edgeId,
               "candidate direction index at v must be reciprocal");

        conflictEntries += static_cast<int>(
            sample3.conflictEdges[edgeId].size());
        for (int other : sample3.conflictEdges[edgeId]) {
            const auto reverse = std::find(
                sample3.conflictEdges[other].begin(),
                sample3.conflictEdges[other].end(), edgeId);
            Expect(reverse != sample3.conflictEdges[other].end(),
                   "conflict adjacency must be symmetric");
        }
    }
    Expect(conflictEntries == 2 * sample3.conflictPairCount,
           "each conflict pair must appear once in each direction");

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

    // A zero matrix is a valid empty design rather than an infeasible input.
    // It must not create phantom vertices, candidates, or conflicts.
    const recover::Model empty = BuildFromText(
        "2 2\n"
        "0 0\n"
        "0 0\n");
    Expect(empty.vertexCount == 0 && empty.CandidateEdgeCount() == 0,
           "empty grid must contain no vertices or candidates");
    Expect(empty.conflictPairCount == 0 && empty.structurallyPossible,
           "empty grid must be structurally feasible");

    // An isolated connector has demand but no candidate edge. Per-vertex
    // capacity can reject this before any component-level reasoning.
    const recover::Model isolated = BuildFromText(
        "2 2\n"
        "1 0\n"
        "0 0\n");
    Expect(!isolated.structurallyPossible,
           "isolated connector must fail the incident-capacity check");
    Expect(isolated.impossibleReason.find("too few") != std::string::npos,
           "isolated connector failure reason must identify capacity");

    // Each row below forms one three-vertex path. Every vertex passes its local
    // incident-capacity check, but each component has odd total demand. This
    // specifically reaches the component-local parity test after global parity
    // and per-vertex capacity have both passed.
    const recover::Model componentParity = BuildFromText(
        "3 6\n"
        "1 1 1 0 0 0\n"
        "0 0 0 1 1 1\n"
        "0 0 0 0 0 0\n");
    Expect(!componentParity.structurallyPossible,
           "component-local odd demand must be rejected");
    Expect(componentParity.impossibleReason.find("component") !=
               std::string::npos,
           "component parity failure reason must identify the component check; "
           "got '" +
               componentParity.impossibleReason + "'");

    // Two degree-four endpoints share only one candidate edge, so the global
    // candidate-count bound is insufficient even though the grid is rectangular.
    const recover::Model tooFewCandidates = BuildFromText(
        "2 2\n"
        "4 0\n"
        "4 0\n");
    Expect(!tooFewCandidates.structurallyPossible,
           "too few candidate edges must be rejected statically");

    // BuildModel distinguishes malformed structural input from a valid but
    // infeasible puzzle. These checks cover empty dimensions, ragged rows, a
    // forbidden one-row grid, and a null output pointer.
    recover::Model invalid;
    std::string error;
    Expect(!recover::BuildModel({}, &invalid, &error),
           "empty model dimensions must be rejected");
    Expect(!recover::BuildModel({{1, 2}, {3}}, &invalid, &error),
           "ragged grid must be rejected");
    Expect(!recover::BuildModel({{1, 2}}, &invalid, &error),
           "one-row grid must be rejected");
    Expect(!recover::BuildModel({{1, 0}, {0, 0}}, nullptr, &error),
           "null model output must be rejected");
    Expect(error == "null model output",
           "null model output must report a concrete diagnostic");
}

// Cover parsing, validation, serialization round trips, and wrong-degree
// rejection. A No Solution fixture checks recognition of the text marker only;
// the marker is not treated as proof of global infeasibility.
void TestValidatorAcceptance() {
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

    std::string windowsOutput = roundTrip.str();
    std::string crlfOutput;
    crlfOutput.reserve(windowsOutput.size() + 16);
    for (char ch : windowsOutput) {
        if (ch == '\n') {
            crlfOutput.push_back('\r');
        }
        crlfOutput.push_back(ch);
    }
    ExpectValidation(sample1, crlfOutput, recover::ValidationStatus::kValid,
                     "valid connector layout", "Windows CRLF output");

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
}

// Exercise every rejection branch in the independent output validator. Exact
// messages make a missing branch visible instead of accepting any failure.
void TestValidatorRejections() {
    // A two-connector row gives one candidate edge and a compact valid witness.
    // Every negative case below changes one property so its rejection branch is
    // reached without introducing unrelated failures first.
    const recover::Model pair = BuildFromText(
        "2 2\n"
        "1 1\n"
        "0 0\n");
    const std::string validPair =
        "1 1 0 0 0 1\n"
        "1 2 0 0 1 0\n";
    ExpectValidation(pair, validPair, recover::ValidationStatus::kValid,
                     "valid connector layout", "validator baseline pair");

    // An empty stream is invalid for a non-empty model; an actual blank line is
    // rejected separately because it represents explicit malformed formatting.
    ExpectValidation(pair, "", recover::ValidationStatus::kInvalid,
                     "empty output", "empty output");
    ExpectValidation(pair, "\n", recover::ValidationStatus::kInvalid,
                     "blank output line", "blank line");

    // Edge whitespace is checked on the raw line before stream extraction,
    // because formatted extraction would silently skip leading spaces.
    ExpectValidation(pair, " 1 1 0 0 0 1\n1 2 0 0 1 0\n",
                     recover::ValidationStatus::kInvalid,
                     "leading or trailing whitespace", "leading whitespace");
    ExpectValidation(pair, "1 1 0 0\n1 2 0 0 1 0\n",
                     recover::ValidationStatus::kInvalid,
                     "malformed connector line", "missing direction field");

    // A seventh token cannot be ignored: doing so could disguise a malformed
    // coordinate stream as a valid six-field connector line.
    ExpectValidation(pair, "1 1 0 0 0 1 9\n1 2 0 0 1 0\n",
                     recover::ValidationStatus::kInvalid,
                     "extra token on connector line", "seventh token");
    ExpectValidation(pair, "9 9 0 0 0 0\n1 2 0 0 1 0\n",
                     recover::ValidationStatus::kInvalid,
                     "connector coordinate out of range",
                     "coordinate outside grid");

    // The matrix contains a real connector only at (1,1) and (1,2). A line for
    // (2,2) must be rejected before it can participate in graph reconstruction.
    ExpectValidation(pair, "2 2 0 0 0 1\n1 1 0 0 0 1\n1 2 0 0 1 0\n",
                     recover::ValidationStatus::kInvalid,
                     "output contains a zero position", "output at a zero cell");
    ExpectValidation(pair, "1 1 0 0 0 1\n1 1 0 0 0 1\n",
                     recover::ValidationStatus::kInvalid, "duplicate connector",
                     "duplicate connector");

    // Vertex ids are assigned row-major, so the second connector cannot be
    // emitted before the first even though both coordinates are individually
    // valid.
    ExpectValidation(pair, "1 2 0 0 1 0\n1 1 0 0 0 1\n",
                     recover::ValidationStatus::kInvalid,
                     "connectors are not in row-major order",
                     "connectors out of order");
    ExpectValidation(pair, "1 1 0 0 0 2\n1 2 0 0 1 0\n",
                     recover::ValidationStatus::kInvalid,
                     "direction flags must be 0 or 1",
                     "non-binary direction");

    // Omitting one connector leaves the model incomplete. This is distinct from
    // an empty output, which fails before any connector has been parsed.
    ExpectValidation(pair, "1 1 0 0 0 1\n",
                     recover::ValidationStatus::kInvalid, "missing connector",
                     "missing connector line");
    ExpectValidation(pair, "1 1 0 0 0 1\n1 2 0 0 0 1\n",
                     recover::ValidationStatus::kInvalid,
                     "fence exists at only one endpoint",
                     "one-sided fence");

    // Both endpoints report degree one, but they do not acknowledge the same
    // candidate edge, so the fenced segment is dangling at one end.
    ExpectValidation(pair, "1 1 0 0 0 0\n1 2 0 0 0 0\n",
                     recover::ValidationStatus::kInvalid,
                     "wrong connector degree", "wrong exact degree");
    ExpectValidation(pair, "No Solution\n1 1 0 0 0 1\n",
                     recover::ValidationStatus::kInvalid,
                     "extra content after No Solution",
                     "content after No Solution");
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
    ExpectValidation(crossing, layout, recover::ValidationStatus::kInvalid,
                     "selected fences cross at an empty cell",
                     "fences crossing at an empty point");
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
    // The validator must accept this exact empty textual representation.
    ExpectValidation(empty, "", recover::ValidationStatus::kValid,
                     "valid empty layout", "all-zero grid");
    // This is distinct from the No Solution marker: the input has a valid empty
    // realization, and no fabricated "No Solution" text is required.
}

void TestValidatorGroup() {
    TestValidatorAcceptance();
    TestValidatorRejections();
    TestIllegalCrossing();
    TestEmptyLayout();
}

void TestRandomDifferentialGroup() {
    TestSolverAgainstBruteForce();
    TestRandomThreeByThreeAndThreeByFour();
}

void TestPropagationGroup() {
    TestForcedPropagation();
    TestConflictPropagation();
}

}  // namespace

namespace {

struct TestGroup {
    const char* name;
    void (*run)();
};

// The registry order is also the full-suite execution order. Parser and model
// groups run before solver groups so construction failures are reported before
// they can turn into misleading search disagreements.
//
// exhaustive-small runs a smaller number of generated models than the random
// group but covers every legal 2x2 and 2x3 matrix. random-differential uses
// fixed seeds and a separate brute-force mask loop. planted starts from a
// hidden valid edge set, while geometry contains hand-derived boundary cases.
//
// CTest invokes one name at a time. Keeping the dispatcher here avoids a second
// test binary and ensures every granular entry exercises exactly the compiled
// production library.
constexpr TestGroup kTestGroups[] = {
    {"parser", TestParser},
    {"model", TestCandidateModel},
    {"validator", TestValidatorGroup},
    {"samples", TestSolverSamples},
    {"exhaustive-small", TestExhaustiveSmallGrids},
    {"random-differential", TestRandomDifferentialGroup},
    {"planted", TestPlantedFeasibleLayouts},
    {"propagation", TestPropagationGroup},
    {"geometry", TestRequiredEdgeCases},
};

// Print the same registry used for dispatch. This makes an unknown group easy
// to diagnose without maintaining a second hard-coded help list.
void PrintTestGroupList(std::ostream* output) {
    *output << "Test groups:\n";
    for (const TestGroup& group : kTestGroups) {
        *output << "  " << group.name << '\n';
    }
}

// A linear lookup is sufficient for nine groups and keeps the dispatcher
// allocation-free.
const TestGroup* FindTestGroup(const std::string& name) {
    for (const TestGroup& group : kTestGroups) {
        if (name == group.name) {
            return &group;
        }
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    // Reject ambiguous multi-group invocations instead of silently running only
    // the first argument. CTest always supplies at most one group name.
    if (argc > 2) {
        std::cerr << "Usage: recover_tests [--list|<group>]\n";
        PrintTestGroupList(&std::cerr);
        return 2;
    }
    if (argc == 2 && std::string(argv[1]) == "--list") {
        PrintTestGroupList(&std::cout);
        return 0;
    }

    if (argc == 1) {
        // Preserve the original developer workflow: no arguments runs every
        // group, while CTest uses the named path for better failure isolation.
        for (const TestGroup& group : kTestGroups) {
            group.run();
        }
        if (failures != 0) {
            std::cerr << failures << " test(s) failed\n";
            return 1;
        }
        std::cout << "All model, solver, and validation tests passed\n";
        return 0;
    }

    const TestGroup* selected = FindTestGroup(argv[1]);
    // Unknown names are usage errors (status 2), distinct from assertion
    // failures (status 1) and successful test completion (status 0).
    if (selected == nullptr) {
        std::cerr << "Unknown test group: " << argv[1] << '\n';
        PrintTestGroupList(&std::cerr);
        return 2;
    }
    selected->run();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed in group "
                  << selected->name << '\n';
        return 1;
    }
    std::cout << "Test group passed: " << selected->name << '\n';
    return 0;
}
