#include "validator.hpp"

#include <sstream>

namespace recover {
namespace {

// These helpers inspect serialized text and do not reference solver state.
//
// Validation reconstructs selected candidate edges from both endpoint flags.
// This catches coordinate or serialization mistakes that an internal-state
// check could miss.
//
// Checks are ordered from cheapest format validation to full graph checks:
// text shape, connector coverage, exact degrees, endpoint pairing, then conflict.
//
// ValidateOutput is read-only and may be called repeatedly on one model.

// Remove a trailing carriage return produced by Windows text files.
std::string RemoveCarriageReturn(std::string line) {
    // Do not trim spaces here; leading/trailing spaces must be reported.
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

// Confirm that no non-whitespace content follows "No Solution".
bool IsOnlyWhitespace(std::istream* input) {
    char ch = '\0';
    // Keep whitespace visible so stray characters cannot be skipped.
    while (*input >> std::noskipws >> ch) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            return false;
        }
    }
    return true;
}

}  // namespace

// Validate a candidate output in five stages:
// special empty/no-solution forms, line syntax, connector coverage and order,
// exact degrees and endpoint pairing, then empty-cell conflict checks.
ValidationResult ValidateOutput(const Model& model, std::istream* output) {
    // Return a concrete reason for each failure.
    ValidationResult result;
    std::string line;
    if (!std::getline(*output, line)) {
        // An all-zero model has a valid empty representation.
        if (model.vertexCount == 0) {
            result.status = ValidationStatus::kValid;
            result.message = "valid empty layout";
            return result;
        }
        result.message = "empty output";
        return result;
    }
    line = RemoveCarriageReturn(line);

    // The text marker is distinguishable from a constructive answer.
    if (line == "No Solution") {
        if (!IsOnlyWhitespace(output)) {
            result.message = "extra content after No Solution";
            return result;
        }
        result.status = ValidationStatus::kNoSolution;
        result.message = "output claims no solution";
        return result;
    }

    // Store the parsed flags by internal vertex id.
    std::vector<std::array<int, 4>> directions(
        model.vertexCount, std::array<int, 4>{0, 0, 0, 0});
    // seen detects both duplicate and missing connector lines.
    std::vector<bool> seen(model.vertexCount, false);

    // Vertex ids are assigned in row-major order, so the output must follow.
    int expectedVertex = 0;
    // Track whether at least one connector line was consumed.
    bool first = true;
    while (true) {
        // Remove only the carriage-return byte; other whitespace stays visible.
        if (line != RemoveCarriageReturn(line)) {
            line = RemoveCarriageReturn(line);
        }
        if (line.empty()) {
            // Blank lines make the submitted format ambiguous.
            result.message = "blank output line";
            return result;
        }
        // Check edge whitespace before stringstream can skip it.
        if (line.front() == ' ' || line.back() == ' ' ||
            line.front() == '\t' || line.back() == '\t') {
            result.message = "leading or trailing whitespace";
            return result;
        }

        // A connector line contains exactly six integers.
        std::istringstream rowStream(line);
        int row = 0;
        int col = 0;
        std::array<int, 4> flags{};
        if (!(rowStream >> row >> col >> flags[0] >> flags[1] >> flags[2] >>
              flags[3])) {
            // All six fields are mandatory.
            result.message = "malformed connector line";
            return result;
        }
        // flags follow the Direction enum: up, down, left, right.
        std::string extra;
        if (rowStream >> extra) {
            // Any seventh token is malformed.
            result.message = "extra token on connector line";
            return result;
        }

        // Convert one-based input coordinates to zero-based storage.
        --row;
        --col;
        if (row < 0 || row >= model.rows || col < 0 || col >= model.cols) {
            result.message = "connector coordinate out of range";
            return result;
        }

        const int vertex = model.vertexAt[row][col];
        // A vertex id of -1 means this position was zero in the input.
        if (vertex == -1) {
            result.message = "output contains a zero position";
            return result;
        }
        if (seen[vertex]) {
            // Each connector must appear exactly once.
            result.message = "duplicate connector";
            return result;
        }

        while (expectedVertex < model.vertexCount &&
               seen[expectedVertex]) {
            // Skip the consumed row-major prefix.
            ++expectedVertex;
        }
        // The next line must contain the next dense vertex id.
        if (expectedVertex >= model.vertexCount || vertex != expectedVertex) {
            result.message = "connectors are not in row-major order";
            return result;
        }

        // Direction fields are binary.
        for (int direction = 0; direction < 4; ++direction) {
            if (flags[direction] != 0 && flags[direction] != 1) {
                result.message = "direction flags must be 0 or 1";
                return result;
            }
        }
        // Retain the flags for global degree and conflict checks.
        directions[vertex] = flags;
        seen[vertex] = true;
        first = false;

        if (!std::getline(*output, line)) {
            // EOF ends parsing; completeness and degree checks still run.
            break;
        }
    }

    if (first) {
        // A non-empty model requires at least one output line.
        result.message = "no connector lines";
        return result;
    }

    // Check exact incidence count at every connector.
    for (int v = 0; v < model.vertexCount; ++v) {
        if (!seen[v]) {
            result.message = "missing connector";
            return result;
        }
        // The number of one bits is the achieved connector degree.
        int degree = 0;
        for (int direction = 0; direction < 4; ++direction) {
            degree += directions[v][direction];
        }
        if (degree != model.requiredDegree[v]) {
            result.message = "wrong connector degree";
            return result;
        }
    }

    // A candidate edge exists only when both reciprocal endpoint flags are one.
    std::vector<bool> selected(model.CandidateEdgeCount(), false);
    for (int edgeId = 0; edgeId < model.CandidateEdgeCount(); ++edgeId) {
        const CandidateEdge& edge = model.edges[edgeId];
        const int fromU = directions[edge.u][edge.directionAtU];
        const int fromV = directions[edge.v][edge.directionAtV];
        if (fromU != fromV) {
            // One-sided acceptance would create a dangling fence.
            result.message = "fence exists at only one endpoint";
            return result;
        }
        selected[edgeId] = fromU == 1;
        // Both zero is also consistent: the candidate is simply absent.
    }

    // The conflict table was precomputed; check every selected pair.
    for (int edgeId = 0; edgeId < model.CandidateEdgeCount(); ++edgeId) {
        if (!selected[edgeId]) {
            continue;
        }
        for (int other : model.conflictEdges[edgeId]) {
            if (selected[other]) {
                result.message = "selected fences cross at an empty cell";
                return result;
            }
        }
    }

    // All structural constraints have now passed.
    result.status = ValidationStatus::kValid;
    result.message = "valid connector layout";
    result.directions = std::move(directions);
    return result;
}

}  // namespace recover
