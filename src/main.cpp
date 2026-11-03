#include "io.hpp"
#include "model.hpp"
#include "solver.hpp"
#include "validator.hpp"

#include <fstream>
#include <iostream>
#include <string>

namespace {

// The executable serves both the submitted solver and developer diagnostics.
// Usage text goes to stderr so standard output contains only the answer.
// --model-info reports candidate-model statistics without searching.
// --validate checks a supplied answer against an input instance.
//
// Invalid command shapes print usage and return a non-zero exit code.
void PrintUsage() {
    std::cerr
        << "Usage:\n"
        << "  recover < input\n"
        << "  recover --validate <input> <output>\n"
        << "  recover --model-info <input>\n";
}

}  // namespace

int main(int argc, char** argv) {
    // The input contains at most 2500 integers, so standard streams suffice.
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    // Exit codes: 0 success, 1 invalid answer, 2 usage/input error,
    // and 3 for a supplied answer that says No Solution.
    if (argc != 1 && argc != 3 && argc != 4) {
        PrintUsage();
        return 2;
    }

    if (argc == 1) {
        // Submitted mode reads the sole test case from stdin.
        recover::Grid grid;
        std::string error;
        if (!recover::ReadSingleCase(&std::cin, &grid, &error)) {
            std::cerr << error << '\n';
            return 2;
        }

        recover::Model model;
        if (!recover::BuildModel(grid, &model, &error)) {
            std::cerr << error << '\n';
            return 2;
        }

        recover::Solver solver(model);
        const recover::SolveResult result = solver.Solve();
        // The solver returns false only after an exact exhaustive search.
        if (result.found) {
            recover::PrintDirections(model, result.directions, &std::cout);
        } else {
            recover::PrintNoSolution(&std::cout);
        }
        return 0;
    }

    const std::string mode = argv[1];
    // Diagnostic modes read an explicit input file.
    std::ifstream inputFile(argv[2]);
    if (!inputFile) {
        std::cerr << "Cannot open input file\n";
        return 2;
    }

    recover::Grid grid;
    std::string error;
    // Parsing checks syntax and ranges, not problem feasibility.
    if (!recover::ReadSingleCase(&inputFile, &grid, &error)) {
        std::cerr << error << '\n';
        return 2;
    }

    recover::Model model;
    // Model construction creates candidates and conflicts without searching.
    if (!recover::BuildModel(grid, &model, &error)) {
        std::cerr << error << '\n';
        return 2;
    }

    if (mode == "--model-info" && argc == 3) {
        // Print model statistics for construction debugging.
        std::cout << "rows " << model.rows << '\n'
                  << "cols " << model.cols << '\n'
                  << "connectors " << model.vertexCount << '\n'
                  << "candidate_edges " << model.CandidateEdgeCount() << '\n'
                  << "conflict_pairs " << model.conflictPairCount << '\n'
                  << "structurally_possible "
                  << (model.structurallyPossible ? 1 : 0) << '\n';
        return 0;
    }

    if (mode == "--validate" && argc == 4) {
        // Validation requires both the input and a candidate output file.
        std::ifstream outputFile(argv[3]);
        if (!outputFile) {
            std::cerr << "Cannot open output file\n";
            return 2;
        }
        // No Solution is a text status, not proof of infeasibility.
        const recover::ValidationResult validation =
            recover::ValidateOutput(model, &outputFile);
        // Diagnostic output is separate from the submitted answer path.
        std::cout << validation.message << '\n';
        // Preserve the documented diagnostic exit-code semantics.
        if (validation.status == recover::ValidationStatus::kValid) {
            return 0;
        }
        if (validation.status == recover::ValidationStatus::kNoSolution) {
            return 3;
        }
        return 1;
    }

    PrintUsage();
    return 2;
}
