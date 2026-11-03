#pragma once

#include "io.hpp"

#include <array>
#include <string>
#include <vector>

namespace recover {

// This file defines the static candidate model.
// It describes selectable straight fences and pairwise crossing conflicts,
// but deliberately contains no DFS state; Solver owns that dynamic state.
//
// Data flow: input matrix -> connector vertices -> candidate segments ->
// empty-cell conflicts.
//
// Naming conventions:
// vertexCount is the number of connectors, not grid cells;
// edgeCount is the number of candidates, not the final selected set;
// directionEdge and incidentEdges query edges from a vertex;
// conflictEdges queries conflicting edges from an edge.
// Internal indices are zero-based; output coordinates are converted later.

// Direction values match the required output order.
enum Direction : int {
    kUp = 0,
    kDown = 1,
    kLeft = 2,
    kRight = 3,
};

// A candidate segment joins consecutive non-zero connectors in a row or column.
// Empty cells may lie inside the segment, so ordinary cell adjacency is not
// sufficient. directionAtU and directionAtV point toward the opposite endpoint.
struct CandidateEdge {
    // First endpoint in the generating row/column scan.
    int u = -1;

    // Second endpoint in that scan.
    int v = -1;

    // Direction from each endpoint toward the other endpoint.
    int directionAtU = 0;
    int directionAtV = 0;

    // Original coordinates are used when scanning the segment's interior.
    int row1 = 0;
    int col1 = 0;
    int row2 = 0;
    int col2 = 0;

    // Equal endpoint rows identify a horizontal segment.
    bool IsHorizontal() const {
        return row1 == row2;
    }
};

struct Model {
    // Original grid dimensions and aggregate model statistics.
    int rows = 0;
    int cols = 0;

    // Number of non-zero connectors.
    int vertexCount = 0;

    // Every selected edge contributes two degree units.
    int totalRequiredDegree = 0;

    // Number of empty-cell conflict pairs.
    int conflictPairCount = 0;

    // Preserve the input and map occupied cells to dense vertex ids.
    Grid grid;

    // A value of -1 denotes an empty position; other values index vertex arrays.
    std::vector<std::vector<int>> vertexAt;

    // Inverse coordinate lookup indexed by vertex id.
    std::vector<int> vertexRow;
    std::vector<int> vertexCol;

    // Exact target degree for each connector.
    std::vector<int> requiredDegree;

    // Candidate id in each direction, or -1 when no connector lies that way.
    // Reciprocal directions identify the same candidate.
    std::vector<std::array<int, 4>> directionEdge;

    // Compact incident-edge list, containing at most four ids.
    std::vector<std::vector<int>> incidentEdges;

    // Stable candidate-edge collection; selection state lives in Solver.
    std::vector<CandidateEdge> edges;

    // Symmetric conflict graph for perpendicular segments crossing at zeros.
    std::vector<std::vector<int>> conflictEdges;

    // A false value proves infeasibility; true is only a necessary-condition
    // pass and never permits skipping search.
    bool structurallyPossible = true;

    // First static failure reason for diagnostics.
    std::string impossibleReason;

    // Typed accessor for the candidate count.
    int CandidateEdgeCount() const {
        return static_cast<int>(edges.size());
    }
};

// Build all vertices, candidate segments, and crossing conflicts.
// Structural input errors return false; a valid but infeasible instance still
// builds successfully.
bool BuildModel(const Grid& input, Model* model, std::string* error);

}  // namespace recover
