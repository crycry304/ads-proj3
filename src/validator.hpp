#pragma once

#include "model.hpp"

#include <array>
#include <istream>
#include <string>
#include <vector>

namespace recover {

// The validator is deliberately independent of solver internals.
// It receives only the input-derived Model and a textual candidate answer.
// It recomputes degrees, endpoint agreement, ordering, and crossings.
//
// ValidationResult separates machine control, feedback, and reconstructed data:
// status drives program logic, message is human-readable, and directions are
// returned only for a valid layout.

// kNoSolution recognizes a claim only; it is not a proof of infeasibility.
enum class ValidationStatus {
    // The candidate layout passed every constraint.
    kValid,

    // The text says No Solution; absence itself is proved by the solver.
    kNoSolution,

    // At least one format, degree, endpoint, or conflict rule failed.
    kInvalid,
};

// A valid result also returns the parsed directions for round-trip testing.
struct ValidationResult {
    // Machine-readable outcome and a concise diagnostic explanation.
    ValidationStatus status = ValidationStatus::kInvalid;
    std::string message;

    // Populated only for kValid.
    std::vector<std::array<int, 4>> directions;
};

// Validate without reading selected/rejected arrays from the solver.
ValidationResult ValidateOutput(const Model& model, std::istream* output);

}  // namespace recover
