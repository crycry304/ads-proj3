#pragma once

#include <array>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace recover {

// The IO module only converts between contest text and internal data.
// It neither decides feasibility nor participates in candidate search.
//
// Input: parse one Grid from a stream.
// Output: serialize connector directions in the required line format.
// Keeping these conversions here avoids duplicated coordinate handling.
//
// Internal coordinates are zero-based; output coordinates are one-based.
// Direction slots are ordered up, down, left, right.
// A one-bit records an incident fence at this connector. Reciprocal
// agreement is checked by the candidate model and validator.

// Grid directly represents the n by m connector-degree matrix.
using Grid = std::vector<std::vector<int>>;
struct Model;

// Parse the single test case in the input.
// On failure, fill error when provided and leave the caller's grid untouched.
bool ReadSingleCase(std::istream* input, Grid* grid, std::string* error);

// Print every non-zero connector in row-major order.
// directions[v] stores the four flags for internal vertex v.
void PrintDirections(const Model& model,
                     const std::vector<std::array<int, 4>>& directions,
                     std::ostream* output);

// Centralize the exact no-solution marker required by the statement.
void PrintNoSolution(std::ostream* output);

}  // namespace recover
