#include "io.hpp"
#include "model.hpp"

#include <sstream>

namespace recover {

// Parsing proceeds in a strict order:
// 1. read and validate n and m;
// 2. read exactly n*m integers;
// 3. validate every degree in [0,4];
// 4. reject trailing tokens;
// 5. publish the grid only after every check succeeds.
//
// A local parsed grid prevents partially initialized state on failure.
//
// Output belongs here as well. It serializes already-decided directions and
// does not attempt to repair degrees or crossings. The validator independently
// checks the same serialized form.
bool ReadSingleCase(std::istream* input, Grid* grid, std::string* error) {
    // error is optional. The input contains one test case, not a test count.
    int rows = 0;
    int cols = 0;
    // Formatted extraction skips ordinary whitespace between integers.
    if (!(*input >> rows >> cols)) {
        // No matrix can follow if the dimensions are missing.
        if (error != nullptr) {
            *error = "missing n or m";
        }
        return false;
    }

    if (rows <= 1 || rows > 50 || cols <= 1 || cols > 50) {
        // Validate dimensions before allocating the matrix.
        if (error != nullptr) {
            *error = "n and m must satisfy 2 <= n,m <= 50";
        }
        return false;
    }

    Grid parsed(rows, std::vector<int>(cols, 0));
    // Read every cell and reject missing, non-numeric, or out-of-range values.
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            // Failure can mean premature EOF or a non-numeric token.
            if (!(*input >> parsed[r][c])) {
                if (error != nullptr) {
                    *error = "grid is shorter than n*m";
                }
                return false;
            }
            // A connector has at most four incident directions.
            if (parsed[r][c] < 0 || parsed[r][c] > 4) {
                if (error != nullptr) {
                    *error = "every connector degree must be in [0, 4]";
                }
                return false;
            }
            // parsed[r][c] is now a validated degree value.
        }
    }

    // The statement allows exactly one case, so trailing data is an error.
    std::string extra;
    if (*input >> extra) {
        if (error != nullptr) {
            *error = "extra token after the single test case";
        }
        return false;
    }

    // Transfer ownership only after complete validation.
    *grid = std::move(parsed);
    return true;
}

// Output depends only on the static Model and the chosen directions.
// vertexAt maps dense vertex ids back to grid coordinates.
void PrintDirections(const Model& model,
                     const std::vector<std::array<int, 4>>& directions,
                     std::ostream* output) {
    // Row-major order is part of the required output format.
    for (int r = 0; r < model.rows; ++r) {
        for (int c = 0; c < model.cols; ++c) {
            const int vertex = model.vertexAt[r][c];
            // A vertex id of -1 marks an empty position to skip.
            if (vertex == -1) {
                continue;
            }

            // Convert zero-based internal coordinates to one-based output.
            *output << (r + 1) << ' ' << (c + 1);

            // Print the already-decided up/down/left/right flags directly.
            for (int direction = 0; direction < 4; ++direction) {
                // Use the exact format: row col up down left right.
                *output << ' ' << directions[vertex][direction];
            }
            // End the line without emitting trailing spaces.
            *output << '\n';
        }
    }
}

// Keep the exact no-solution text in one place.
void PrintNoSolution(std::ostream* output) {
    *output << "No Solution\n";
}

}  // namespace recover
