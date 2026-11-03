#pragma once

#include "model.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace recover {

// Search statistics expose the cost of search and propagation for experiments.
// Nodes count recursive entries; backtracks count failed and rolled-back nodes.
struct SearchStats {
    // Increment for every Search call, including nodes that fail immediately.
    std::uint64_t nodes = 0;

    // Count a node after all of its completion subsets have failed.
    std::uint64_t backtracks = 0;

    // Number of connector inspections consumed from the propagation queue.
    // A connector may be queued more than once as adjacent states change.
    std::uint64_t propagationEvents = 0;

    // Unknown edges rejected because their endpoint is already satisfied.
    std::uint64_t forcedRejections = 0;

    // Unknown edges selected because all remaining choices are required.
    std::uint64_t forcedSelections = 0;

    // Edges rejected after conflicting with an already selected edge.
    std::uint64_t conflictRejections = 0;

    // Deepest connector decision layer reached by the search.
    int maximumDepth = 0;
};

// found distinguishes a constructive answer from a completed infeasibility proof.
struct SolveResult {
    // False means the exhaustive search finished without finding a layout.
    bool found = false;

    // Direction flags indexed by internal vertex id when found is true.
    std::vector<std::array<int, 4>> directions;

    // Statistics remain available for both feasible and infeasible instances.
    SearchStats stats;
};

// The solver combines complete backtracking, queued propagation, and MRV
// variable ordering. Earlier Git revisions provide comparison baselines.
//
// A search node stores a three-way assignment for every candidate edge. The
// assignment is partial: kUnknown means that a future branch may still choose
// the edge, while kSelected and kRejected are fixed on the current path. The
// solver never changes the static Model, so branching state consists only of
// edgeStates_, the two degree counters, the queue, and the rollback trail.
//
// The queue is an incremental consistency mechanism rather than a second search
// engine. AssignEdge queues an edge's endpoints, and Propagate repeatedly
// applies the two exact-degree implications until no counter changes. Rollback
// restores the counter and trail snapshots after a failed child and starts the
// next child with an empty event queue.
//
// MRV is a heuristic variable-ordering rule over connectors. It can reduce the
// number of visited nodes, but it does not make node counts monotone across
// every input and its conflict-weight scan has a small per-node cost. The
// benchmark comparison therefore reports both tree size and elapsed time rather
// than assuming that fewer local choices always imply a faster run.
//
// A Solver is intended for one Solve call. Reusing partially explored state
// would make test isolation fragile; construct a fresh Solver for a new model.
class Solver {
public:
    // The model reference is immutable during search.
    explicit Solver(const Model& model);

    // Return the first valid layout, or prove that no layout exists.
    SolveResult Solve();

private:
    // Private methods maintain edge states, rollback, and local completion
    // subsets. Callers depend only on SolveResult, not internal edge ids.
    // UNKNOWN: not decided on the current branch;
    // REJECTED: confirmed absent on the current branch;
    // SELECTED: confirmed present on the current branch.
    enum EdgeState : std::uint8_t {
        kUnknown = 0,
        kRejected = 1,
        kSelected = 2,
    };

    // Each trail entry records one UNKNOWN-to-decided transition.
    struct TrailEntry {
        // Candidate edge whose state changed.
        int edgeId = -1;

        // State restored during rollback; currently always kUnknown.
        EdgeState previousState = kUnknown;
    };

    // A branch fills one connector's remaining degree with a subset of its
    // currently unknown candidate edges.
    struct BranchChoice {
        // True when no legal degree-completing subset exists.
        bool impossible = false;

        // Connector chosen as the variable for this node.
        int vertex = -1;

        // Remaining degree at vertex.
        int need = 0;

        // Incident candidates that are still unknown.
        std::vector<int> availableEdges;

        // Number of completion subsets: C(availableEdges.size(), need).
        int combinationCount = 0;

        // Sum of conflict degrees, used as a deterministic MRV tie-breaker.
        int conflictWeight = 0;
    };

    bool AssignEdge(int edgeId, EdgeState newState);
    bool Propagate();
    BranchChoice ChooseBranch();
    bool Search(int depth);
    bool IsComplete() const;
    bool TryCombination(const BranchChoice& choice,
                        const std::vector<int>& combination, int depth);
    void Rollback(std::size_t checkpoint);
    void EnumerateCombinations(const std::vector<int>& availableEdges, int need,
                               int start, std::vector<int>* current,
                               std::vector<std::vector<int>>* combinations) const;
    void MaterializeDirections();

    const Model& model_;

    // Indexed by stable candidate-edge id.
    std::vector<EdgeState> edgeStates_;

    // Number of selected incident candidates for each connector.
    std::vector<int> selectedDegree_;

    // Number of unknown incident candidates. If selected + unknown is below the
    // required degree, the current branch cannot be completed.
    std::vector<int> unknownDegree_;

    // FIFO event queue. Both endpoints of a changed edge are queued, and entries
    // are consumed until propagation reaches a fixed point. Entries are vertex
    // ids because a change in either incident edge can alter the same degree
    // equations. They are not candidate ids and need not be unique.
    std::vector<int> pendingVertices_;
    std::size_t pendingHead_ = 0;

    // Incremental state changes along the current search path.
    std::vector<TrailEntry> trail_;

    // Accumulates the answer and statistics during recursive search.
    SolveResult result_;
};

}  // namespace recover
