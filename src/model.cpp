#include "model.hpp"

#include <queue>

namespace recover {
namespace {

// This file builds the static candidate model and performs no search.
// The construction order is significant:
// 1. validate the matrix shape;
// 2. assign dense ids to non-zero connectors;
// 3. create horizontal and vertical candidate segments;
// 4. apply inexpensive parity and capacity checks;
// 5. scan empty cells to build the symmetric conflict graph.
//
// Consecutive non-zero connectors define one long candidate segment. For
// example, "1 0 0 2 0 3" contains horizontal candidates 1--2 and 2--3, not an
// edge for every pair of adjacent cells. A zero passed through by the segment
// does not introduce a connector or a bend.
//
// A horizontal and a vertical segment conflict exactly when both pass through
// the same empty cell. The relation is stored in both adjacency lists so that
// selecting or rejecting either endpoint of the conflict can propagate.
//
// This representation turns the original geometric task into an exact-degree
// selection problem over a sparse graph:
// - vertices are the original non-zero connector positions;
// - candidate edges are maximal straight runs between consecutive connectors;
// - each vertex requires an exact number of selected incident edges;
// - each conflict pair forbids selecting both perpendicular candidates.
//
// The model itself proves only local or aggregate necessary conditions. It does
// not choose a subset, so feasibility still requires the complete solver.

// Add one undirected candidate segment and maintain every derived index.
// directionAtU and directionAtV are reciprocal: horizontal candidates use
// (right, left), and vertical candidates use (down, up). If the directions are
// inconsistent, the validator will later observe a fence at only one endpoint.
int AddEdge(Model* model, int u, int v, int directionAtU, int directionAtV) {
    // Vertex ids were assigned in row-major order, so their coordinates can be
    // recovered from the inverse lookup arrays.
    CandidateEdge edge;
    // Keep endpoint ids, reciprocal directions, and coordinates together; these
    // fields are sufficient to restore both the output and conflict geometry.
    edge.u = u;
    edge.v = v;
    edge.directionAtU = directionAtU;
    edge.directionAtV = directionAtV;
    edge.row1 = model->vertexRow[u];
    edge.col1 = model->vertexCol[u];
    edge.row2 = model->vertexRow[v];
    edge.col2 = model->vertexCol[v];
    // Horizontal candidates have row1 == row2; vertical candidates have
    // col1 == col2. The coordinates also delimit the segment interior.

    // Ids follow construction order and remain stable for search and validation.
    const int edgeId = static_cast<int>(model->edges.size());
    model->edges.push_back(edge);
    // Maintain both vertex-to-edge lists and exact direction lookups.
    model->incidentEdges[u].push_back(edgeId);
    model->incidentEdges[v].push_back(edgeId);
    model->directionEdge[u][directionAtU] = edgeId;
    model->directionEdge[v][directionAtV] = edgeId;
    return edgeId;
}

// Check necessary conditions independently on every candidate component.
// Edges cannot transfer degree demand between components, so each component
// needs an even total demand and at least demand / 2 available candidates.
bool CheckCandidateComponents(Model* model) {
    // Store a representative start vertex for each component; -1 means unseen.
    std::vector<int> component(model->vertexCount, -1);
    for (int start = 0; start < model->vertexCount; ++start) {
        // Skip vertices already assigned by an earlier BFS.
        if (component[start] != -1) {
            continue;
        }

        std::queue<int> pending;
        // Accumulate demand and incidence within this component only.
        pending.push(start);
        component[start] = start;
        int requiredDegree = 0;
        int endpointOccurrences = 0;

        while (!pending.empty()) {
            // BFS discovers all connectors reachable through candidates.
            const int u = pending.front();
            pending.pop();
            requiredDegree += model->requiredDegree[u];

            // Each candidate is seen once from each endpoint.
            for (int edgeId : model->incidentEdges[u]) {
                // Convert the two endpoint visits into one edge count below.
                ++endpointOccurrences;
                const CandidateEdge& edge = model->edges[edgeId];
                const int v = edge.u == u ? edge.v : edge.u;
                if (component[v] == -1) {
                    // Mark a newly reached connector and continue the BFS.
                    component[v] = start;
                    pending.push(v);
                }
            }
        }

        // Converts two endpoint visits per edge into the actual edge count.
        const int componentEdgeCount = endpointOccurrences / 2;
        if (requiredDegree % 2 != 0 ||
            componentEdgeCount < requiredDegree / 2) {
            // These checks do not depend on the conflict graph.
            model->structurallyPossible = false;
            model->impossibleReason =
                "a candidate component cannot satisfy its degree demand";
            return false;
        }
    }
    return true;
}

}  // namespace

bool BuildModel(const Grid& input, Model* model, std::string* error) {
    // BuildModel constructs static data only; selection state belongs to Solver.
    // Rebuilding an instance must reproduce every id and conflict exactly.
    //
    // Postconditions:
    // - the non-negative entries of vertexAt are exactly [0, vertexCount);
    // - each candidate appears once in each endpoint's incidentEdges list;
    // - each direction has at most one candidate;
    // - conflictEdges is symmetric;
    // - a candidate id equals its index in edges.
    //
    // Vertex and candidate ids are deterministic function of row-major input
    // order. Tests and the benchmark comparison rely on this without treating
    // ids as semantic labels in the output.
    // Reject a null output pointer before touching caller-owned state.
    if (model == nullptr) {
        if (error != nullptr) {
            *error = "null model output";
        }
        return false;
    }

    Model built;
    // Every later stage assumes a rectangular grid with valid dimensions.
    built.grid = input;
    built.rows = static_cast<int>(input.size());
    built.cols = built.rows == 0 ? 0 : static_cast<int>(input[0].size());
    if (built.rows <= 1 || built.cols <= 1) {
        if (error != nullptr) {
            *error = "grid dimensions are invalid";
        }
        return false;
    }
    for (const auto& row : input) {
        // Uniform row lengths make flattened indexing safe.
        if (static_cast<int>(row.size()) != built.cols) {
            if (error != nullptr) {
                *error = "grid is not rectangular";
            }
            return false;
        }
    }

    // Assign dense ids to connectors and keep -1 at empty positions.
    built.vertexAt.assign(built.rows, std::vector<int>(built.cols, -1));
    for (int r = 0; r < built.rows; ++r) {
        for (int c = 0; c < built.cols; ++c) {
            const int degree = input[r][c];
            if (degree < 0 || degree > 4) {
                if (error != nullptr) {
                    *error = "connector degree is outside [0, 4]";
                }
                return false;
            }
            if (degree == 0) {
                // Empty cells are not vertices.
                continue;
            }

            // Row-major ids make the required output order unambiguous.
            const int id = built.vertexCount++;
            // Ids are contiguous and index all vertex-owned arrays directly.
            built.vertexAt[r][c] = id;
            built.vertexRow.push_back(r);
            built.vertexCol.push_back(c);
            built.requiredDegree.push_back(degree);
            built.totalRequiredDegree += degree;
        }
    }

    // Initialize direction lookup slots before candidate construction.
    built.incidentEdges.resize(built.vertexCount);
    built.directionEdge.assign(
        built.vertexCount, std::array<int, 4>{-1, -1, -1, -1});

    // Consecutive non-zero connectors in each row define horizontal candidates.
    // Skipping zeros allows a candidate to span any number of empty columns.
    // Every row scan contributes at most one candidate between two consecutive
    // connectors; the vertical scan adds the symmetric column candidates.
    for (int r = 0; r < built.rows; ++r) {
        // Remember the previous connector so intervening zeros remain interior
        // cells of one long segment.
        int previous = -1;
        for (int c = 0; c < built.cols; ++c) {
            const int current = built.vertexAt[r][c];
            if (current == -1) {
                // Zeros affect segment length, not its endpoints.
                continue;
            }
            if (previous != -1) {
                // The two reciprocal flags identify the same undirected edge.
                AddEdge(&built, previous, current, kRight, kLeft);
            }
            // The current connector becomes the left endpoint of the next pair.
            previous = current;
        }
    }

    // The vertical construction is symmetric and may also span empty cells.
    for (int c = 0; c < built.cols; ++c) {
        // Scan from top to bottom while retaining the previous connector.
        int previous = -1;
        for (int r = 0; r < built.rows; ++r) {
            const int current = built.vertexAt[r][c];
            if (current == -1) {
                continue;
            }
            if (previous != -1) {
                // The upper endpoint points down and the lower points up.
                AddEdge(&built, previous, current, kDown, kUp);
            }
            // The current connector becomes the next upper endpoint.
            previous = current;
        }
    }

    // Each selected fence contributes two degree units. Therefore odd total
    // demand is impossible, as is having fewer candidates than demand / 2.
    if (built.totalRequiredDegree % 2 != 0 ||
        built.CandidateEdgeCount() < built.totalRequiredDegree / 2) {
        // Retain the first reason for diagnostics and tests.
        built.structurallyPossible = false;
        built.impossibleReason =
            "odd total degree or too few candidate edges";
    }

    for (int v = 0; v < built.vertexCount; ++v) {
        // No completion exists if a connector has fewer candidates than demand.
        if (static_cast<int>(built.incidentEdges[v].size()) <
            built.requiredDegree[v]) {
            built.structurallyPossible = false;
            // Preserve the first concrete failure reason.
            built.impossibleReason =
                "a connector has too few possible incident segments";
            break;
        }
    }

    if (built.structurallyPossible) {
        // Component checks are useful only while the global checks still pass.
        CheckCandidateComponents(&built);
    }

    // Candidate intervals in one row or column do not overlap. Thus an empty
    // cell is crossed by at most one horizontal and one vertical candidate.
    // Keeping one id per direction avoids an O(E_horiz * E_vert) all-pairs scan.
    // Each cell then reveals at most one conflict pair in linear grid time.
    std::vector<int> horizontalAt(built.rows * built.cols, -1);
    // Both lookup arrays use the flattened index row * cols + col.
    std::vector<int> verticalAt(built.rows * built.cols, -1);
    for (int edgeId = 0; edgeId < built.CandidateEdgeCount(); ++edgeId) {
        const CandidateEdge& edge = built.edges[edgeId];
        if (edge.IsHorizontal()) {
            // Mark strict interior cells; endpoint connectors are never empty.
            for (int c = edge.col1 + 1; c < edge.col2; ++c) {
                horizontalAt[edge.row1 * built.cols + c] = edgeId;
            }
        } else {
            // Vertical candidates mark only rows strictly between endpoints.
            for (int r = edge.row1 + 1; r < edge.row2; ++r) {
                verticalAt[r * built.cols + edge.col1] = edgeId;
            }
        }
    }

    // Perpendicular crossing is forbidden only at empty cells. A connector may
    // join or turn the fences that meet there.
    // Marking only the strict interior of each segment ensures that an endpoint
    // never becomes a geometric conflict merely because another fence reaches
    // the same connector.
    built.conflictEdges.resize(built.CandidateEdgeCount());
    for (int r = 0; r < built.rows; ++r) {
        for (int c = 0; c < built.cols; ++c) {
            // A connector permits perpendicular incidence, so it is not a conflict.
            if (built.grid[r][c] != 0) {
                continue;
            }
            const int horizontal = horizontalAt[r * built.cols + c];
            const int vertical = verticalAt[r * built.cols + c];
            if (horizontal != -1 && vertical != -1) {
                // Opposing horizontal and vertical crossings are an undirected pair.
                built.conflictEdges[horizontal].push_back(vertical);
                built.conflictEdges[vertical].push_back(horizontal);
                ++built.conflictPairCount;
            }
        }
    }

    // Publish the completed model without copying its grid and candidate arrays.
    *model = std::move(built);
    return true;
}

}  // namespace recover
