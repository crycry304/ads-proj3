#include "io.hpp"
#include "model.hpp"
#include "solver.hpp"
#include "validator.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Case generation lives in tools/generate_cases.py so every historical solver
// version can be measured on the same files. This executable only orchestrates
// model construction, solving, validation, and CSV output.

// Read one input file. The path may contain spaces and is used only for file IO;
// CSV rows contain the stem rather than a local absolute path.
bool LoadCase(const std::string& path, recover::Grid* grid,
              std::string* error) {
    std::ifstream input(path);
    if (!input) {
        *error = "cannot open " + path;
        return false;
    }
    return recover::ReadSingleCase(&input, grid, error);
}

// Build and solve one instance, then validate every constructive answer with
// the independent validator. wall_ms covers BuildModel and Solver::Solve only;
// file IO, text serialization, and validation are intentionally excluded.
std::string RunOnce(const std::string& path, bool check_answer,
                    recover::SearchStats* stats, bool* found, double* wall_ms) {
    // Load input before starting the clock.
    recover::Grid grid;
    std::string error;
    if (!LoadCase(path, &grid, &error)) {
        // Malformed input is an infrastructure error, not an infeasible case.
        return error;
    }

    // Start timing immediately before model construction so preprocessing and
    // search are both included.
    const auto start = std::chrono::steady_clock::now();
    recover::Model model;
    if (!recover::BuildModel(grid, &model, &error)) {
        // Model construction failure must be reported, not encoded as found=0.
        return error;
    }
    recover::Solver solver(model);
    const recover::SolveResult result = solver.Solve();
    const auto finish = std::chrono::steady_clock::now();

    // Preserve solver-reported counters directly.
    *stats = result.stats;
    *found = result.found;
    *wall_ms = std::chrono::duration<double, std::milli>(finish - start).count();

    if (check_answer && result.found) {
        // Exercise the production serializer before independent validation.
        std::ostringstream answer;
        recover::PrintDirections(model, result.directions, &answer);
        std::istringstream answerInput(answer.str());
        const recover::ValidationResult validation =
            recover::ValidateOutput(model, &answerInput);
        if (validation.status != recover::ValidationStatus::kValid) {
            return "solver output failed validation: " + validation.message;
        }
    }
    return {};
}

void PrintHeader() {
    // Field order is stable for tools/run_benchmarks.py.
    // case: input stem without a directory or extension;
    // n,m: original grid dimensions;
    // connectors: number of non-zero connectors;
    // candidate_edges: number of straight candidate segments;
    // conflict_pairs: empty-cell crossing pairs;
    // found: whether a valid layout was found;
    // nodes and backtracks: search-tree activity;
    // propagation_events, forced_*, and conflict_rejections: pruning activity;
    // max_depth: deepest decision layer reached;
    // wall_ms: one model-build-plus-solve wall-clock measurement.
    std::cout
        << "case,n,m,connectors,candidate_edges,conflict_pairs,found,"
           "nodes,backtracks,propagation_events,forced_selections,"
           "forced_rejections,conflict_rejections,max_depth,wall_ms\n";
}

void PrintRow(const std::string& path, const recover::Model& model,
              const recover::SearchStats& stats, bool found, double wallMs) {
#if defined(RECOVER_LEGACY_BASELINE)
    // The legacy baseline predates propagation counters. Missing metrics are
    // emitted as zero rather than reconstructed from unrelated data.
    const std::uint64_t propagationEvents = 0;
    const std::uint64_t forcedSelections = 0;
    const std::uint64_t forcedRejections = 0;
    const std::uint64_t conflictRejections = 0;
#else
    const std::uint64_t propagationEvents = stats.propagationEvents;
    const std::uint64_t forcedSelections = stats.forcedSelections;
    const std::uint64_t forcedRejections = stats.forcedRejections;
    const std::uint64_t conflictRejections = stats.conflictRejections;
#endif

    // Use only the file stem so result CSVs contain no absolute paths. Raw
    // wall_ms values are retained for the aggregation script.
    std::cout << std::filesystem::path(path).stem().string() << ','
              << model.rows << ',' << model.cols << ','
              << model.vertexCount << ',' << model.CandidateEdgeCount() << ','
              << model.conflictPairCount << ',' << (found ? 1 : 0) << ','
              << stats.nodes << ',' << stats.backtracks << ','
              << propagationEvents << ',' << forcedSelections
              << ',' << forcedRejections << ','
              << conflictRejections << ',' << stats.maximumDepth << ','
              << wallMs << '\n';
}

recover::Model BuildMetadata(const std::string& path) {
    // Rebuild outside the timed region to report candidate and conflict counts.
    recover::Grid grid;
    std::string error;
    recover::Model model;
    if (!LoadCase(path, &grid, &error) ||
        !recover::BuildModel(grid, &model, &error)) {
        return model;
    }
    return model;
}

}  // namespace

int main(int argc, char** argv) {
    // Measure each file five times after one untimed warm-up run.
    // Five samples are sufficient for the report's median and maximum, while
    // leaving the complete fixed corpus practical on a development machine.
    int repetitions = 5;
    int firstPath = 1;
    if (argc >= 3 && std::string(argv[1]) == "--repeat") {
        // Parse the repeat count; case paths start after it.
        std::istringstream count(argv[2]);
        if (!(count >> repetitions) || repetitions < 1) {
            std::cerr << "Invalid repetition count\n";
            return 2;
        }
        firstPath = 3;
    }
    if (firstPath >= argc) {
        // Reject an empty case list before writing a header-only result.
        std::cerr << "Usage: recover_benchmark [--repeat N] <case>...\n";
        return 2;
    }

    // Print the header once, followed by one row per repeated run. Keeping raw
    // repetitions makes timing variance and failed regressions auditable.
    PrintHeader();
    for (int argument = firstPath; argument < argc; ++argument) {
        // Warm up allocations and file caches without recording a sample.
        const std::string path = argv[argument];

        recover::SearchStats warmStats;
        bool warmFound = false;
        double warmMs = 0.0;
        // Warm-up answers need not be serialized or validated.
        const std::string warmError =
            RunOnce(path, false, &warmStats, &warmFound, &warmMs);
        if (!warmError.empty()) {
            // Infrastructure failures must not be recorded as timing data.
            std::cerr << path << ": " << warmError << '\n';
            return 1;
        }

        for (int repeat = 0; repeat < repetitions; ++repeat) {
            // A fresh Solver isolates each repetition.
            recover::SearchStats stats;
            bool found = false;
            double wallMs = 0.0;
            // Validate measured answers so invalid solutions never enter CSV.
            const std::string error =
                RunOnce(path, true, &stats, &found, &wallMs);
            if (!error.empty()) {
                // Return a non-zero code so the runner records a failed case.
                std::cerr << path << ": " << error << '\n';
                return 1;
            }
            // Metadata is rebuilt outside wall_ms for CSV reporting. It depends
            // only on the fixed input, so its values need not be measured again.
            const recover::Model model = BuildMetadata(path);
            PrintRow(path, model, stats, found, wallMs);
        }
    }
    return 0;
}
