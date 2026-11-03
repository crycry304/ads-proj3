#include "solver.hpp"

#include <algorithm>
#include <limits>

namespace recover {

// The search chooses an unsatisfied connector and commits its remaining local
// completion at once. Since a connector has at most four incident candidates,
// there are at most C(4,2) = 6 subsets to enumerate. Every unknown incident
// candidate outside the chosen subset is then rejected to prevent excess degree.
//
// Event-queue propagation, conflict rejection, and MRV are pruning mechanisms;
// they do not change the set of legal layouts. Each selected edge is checked
// against already selected conflicting edges, and state changes propagate both
// forced selections and forced rejections until a fixed point is reached.
//
// Invariants of a search state:
// - an edge leaves kUnknown only through AssignEdge and returns through Rollback;
// - selectedDegree_[v] equals the number of selected incident candidates;
// - selectedDegree_[v] never exceeds requiredDegree[v];
// - conflictEdges is symmetric, so selected edges are pairwise conflict-free;
// - unknownDegree_[v] equals the number of unknown incident candidates.
//
// Completeness follows from local subset coverage. If a connector still needs r
// edges, every legal completion must select one r-element subset of its unknown
// incident candidates. The algorithm enumerates all such subsets. Fixing one
// subset rejects the connector's other candidates, so recursive branches only
// make additional decisions and cannot lose a legal completion.
//
// The two proof obligations are therefore:
// 1. any reported success satisfies exact degree and conflict constraints;
// 2. exhausting every local subset exhausts every legal completion.
// Propagation and MRV only remove impossible branches or reorder them.
//
// The event queue stores connector ids, not edge ids. Duplicate queue entries
// are intentional: each inspection reads the latest counters, so stale entries
// become no-ops. With at most four candidates per connector, this remains small.
//
// MRV (minimum remaining values) orders variables by the local combination
// count C(unknown, needed). Smaller counts are explored first because they are
// likely to reveal contradictions sooner and trigger conflict propagation
// earlier. Ties prefer a larger conflict weight, then the lower vertex id.
//
// Each child fixes one connector's complete local contribution before the next
// recursive call. This keeps depth bounded by the number of connectors rather
// than by the number of candidates, while TryCombination still enumerates every
// subset exactly once. The trail contains only changes made below its
// checkpoint, so rollback does not need to copy the full search state.

// Initialize edge states and per-connector counters from the static model.
Solver::Solver(const Model& model)
    : model_(model),
      edgeStates_(model.CandidateEdgeCount(), kUnknown),
      selectedDegree_(model.vertexCount, 0),
      unknownDegree_(model.vertexCount, 0) {
    // Every incident candidate starts unknown.
    for (int v = 0; v < model_.vertexCount; ++v) {
        unknownDegree_[v] =
            static_cast<int>(model_.incidentEdges[v].size());
    }
}

// Solve first honors static infeasibility, then starts depth-first search.
// It stops at the first valid layout because the problem accepts any solution.
SolveResult Solver::Solve() {
    // A failed necessary condition is already a complete infeasibility proof.
    if (!model_.structurallyPossible) {
        return result_;
    }

    // An all-zero model is complete immediately through Search(0).
    if (Search(0)) {
        // Convert the successful edge-state assignment into direction arrays.
        result_.found = true;
        MaterializeDirections();
    }
    // Keep statistics for both successful and exhaustive searches.
    return result_;
}

// AssignEdge is the sole state-changing entry point.
// Repeating the current state succeeds; assigning the opposite state after a
// decision is a branch contradiction. Selecting an edge also rejects every
// unknown conflicting edge in the same pass, while failing immediately if a
// conflicting edge was already selected.
bool Solver::AssignEdge(int edgeId, EdgeState newState) {
    // A committed edge cannot be overwritten without first rolling back.
    if (edgeStates_[edgeId] != kUnknown) {
        return edgeStates_[edgeId] == newState;
    }

    // Record the previous state so rollback can undo exactly this assignment.
    trail_.push_back({edgeId, edgeStates_[edgeId]});
    edgeStates_[edgeId] = newState;

    const CandidateEdge& edge = model_.edges[edgeId];

    // Both endpoints lose one unknown incident edge and may now have a forced
    // decision, regardless of whether this edge was selected or rejected.
    --unknownDegree_[edge.u];
    --unknownDegree_[edge.v];
    pendingVertices_.push_back(edge.u);
    pendingVertices_.push_back(edge.v);

    if (newState == kSelected) {
        // An undirected fence contributes one degree unit at each endpoint.
        ++selectedDegree_[edge.u];
        ++selectedDegree_[edge.v];

        // One scan preserves conflict correctness in every build. The submitted
        // build additionally rejects unknown conflicting edges immediately; the
        // comparison build disables that propagation to isolate its value.
        for (int other : model_.conflictEdges[edgeId]) {
            // Simultaneous selections would cross at an empty cell.
            if (edgeStates_[other] == kSelected) {
                return false;
            }
#if !defined(RECOVER_DISABLE_CONFLICT_PROPAGATION)
            if (edgeStates_[other] == kUnknown) {
                ++result_.stats.conflictRejections;
                if (!AssignEdge(other, kRejected)) {
                    return false;
                }
            }
#endif
        }
    }
    return true;
}

// A state is complete when every connector reaches its exact required degree.
// Undecided candidates may remain unknown; their endpoints are already
// satisfied, so they must never be selected later in this branch.
bool Solver::IsComplete() const {
    // Exact degree, not processing all candidate edges, defines completion.
    for (int v = 0; v < model_.vertexCount; ++v) {
        if (selectedDegree_[v] != model_.requiredDegree[v]) {
            return false;
        }
    }
    return true;
}

// Propagate consumes connector events until no forced decision remains.
// For each connector, selected + unknown < required proves branch failure.
// selected == required forces every unknown incident edge to be rejected.
// selected + unknown == required forces every unknown incident edge to be
// selected. Conflict rejection produced by one forced selection queues the
// affected endpoints again, so the loop continues to a fixed point.
// The first forced edge can therefore produce a wave of later events through
// the other selected endpoint, through a conflict rejection, or through both.
bool Solver::Propagate() {
    // pendingHead_ implements a FIFO queue without erasing a consumed prefix.
    while (pendingHead_ < pendingVertices_.size()) {
        const int v = pendingVertices_[pendingHead_++];
        ++result_.stats.propagationEvents;

        const int required = model_.requiredDegree[v];
        const int selected = selectedDegree_[v];
        const int unknown = unknownDegree_[v];

        // Too many selections or too few remaining choices proves failure.
        if (selected > required || selected + unknown < required) {
            return false;
        }

        if (selected == required) {
            // Any additional selection would exceed the target degree.
            for (int edgeId : model_.incidentEdges[v]) {
                if (edgeStates_[edgeId] == kUnknown) {
                    ++result_.stats.forcedRejections;
                    if (!AssignEdge(edgeId, kRejected)) {
                        return false;
                    }
                }
            }
        } else if (selected + unknown == required) {
            // Selecting every remaining unknown edge is the only way to reach
            // the target, so these edges are forced rather than optional.
            for (int edgeId : model_.incidentEdges[v]) {
                if (edgeStates_[edgeId] == kUnknown) {
                    ++result_.stats.forcedSelections;
                    if (!AssignEdge(edgeId, kSelected)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

// ChooseBranch scans every unsatisfied connector, computes its remaining local
// combination count, and returns the MRV choice. The iterative binomial
// recurrence is exact for n <= 4. As soon as any connector has fewer available
// edges than needed, impossible is set and scanning can stop.
// The scan is intentionally repeated at each node: counters change after
// propagation, so reusing an earlier ordering would ignore newly learned
// constraints. This costs a linear pass over connectors but keeps the branch
// choice aligned with the current state.
Solver::BranchChoice Solver::ChooseBranch() {
    BranchChoice best;
    best.combinationCount = std::numeric_limits<int>::max();

#if defined(RECOVER_FIXED_ORDER)
    // The comparison baseline uses the smallest unsatisfied connector id.
    // This path is compiled only for benchmark builds.
    for (int v = 0; v < model_.vertexCount; ++v) {
        const int need = model_.requiredDegree[v] - selectedDegree_[v];
        if (need == 0) {
            continue;
        }

        best.vertex = v;
        best.need = need;
        for (int edgeId : model_.incidentEdges[v]) {
            if (edgeStates_[edgeId] == kUnknown) {
                best.availableEdges.push_back(edgeId);
            }
        }
        if (static_cast<int>(best.availableEdges.size()) < need) {
            best.impossible = true;
        }
        return best;
    }
    return best;
#else
    for (int v = 0; v < model_.vertexCount; ++v) {
        const int need = model_.requiredDegree[v] - selectedDegree_[v];
        if (need == 0) {
            // A satisfied connector has no remaining decision.
            continue;
        }

        std::vector<int> available;
        int conflictWeight = 0;
        for (int edgeId : model_.incidentEdges[v]) {
            if (edgeStates_[edgeId] == kUnknown) {
                available.push_back(edgeId);

                // A candidate may conflict with several perpendicular edges.
                // More conflicts can produce stronger propagation after a choice.
                conflictWeight +=
                    static_cast<int>(model_.conflictEdges[edgeId].size());
            }
        }

        if (static_cast<int>(available.size()) < need) {
            BranchChoice impossible;
            impossible.impossible = true;
            return impossible;
        }

        // Compute C(available.size(), need) with intermediate products that
        // remain divisible at every step.
        int combinations = 1;
        for (int i = 0; i < need; ++i) {
            combinations =
                combinations * (static_cast<int>(available.size()) - i) /
                (i + 1);
        }

        const bool fewerValues =
            combinations < best.combinationCount;
        const bool sameValues =
            combinations == best.combinationCount;
        const bool moreConflicts =
            conflictWeight > best.conflictWeight;
        // Primary key: fewer remaining local completion subsets.
        // Secondary key: larger conflict weight. The ascending scan preserves
        // the lower vertex id on a complete tie and makes results reproducible.
        if (best.vertex == -1 || fewerValues ||
            (sameValues && moreConflicts)) {
            best.vertex = v;
            best.need = need;
            best.availableEdges = std::move(available);
            best.combinationCount = combinations;
            best.conflictWeight = conflictWeight;
        }
    }
    return best;
#endif
}

// Search returns true after finding one complete layout and false only after
// every local completion below the current state has failed.
// depth affects statistics but not constraints. MRV changes variable order,
// not the values explored at each variable.
// A node counts as visited before propagation because even an immediately
// contradictory state is work produced by its parent's decision.
bool Solver::Search(int depth) {
    ++result_.stats.nodes;
    result_.stats.maximumDepth =
        std::max(result_.stats.maximumDepth, depth);

    // Propagate before branching so forced decisions and contradictions are
    // discovered from the newest counters rather than the construction state.
    if (!Propagate()) {
        ++result_.stats.backtracks;
        return false;
    }

    if (IsComplete()) {
        return true;
    }

    // Propagation is now at a fixed point, so MRV sees current counters.
    BranchChoice choice = ChooseBranch();

    // No completion subset or no remaining variable ends this node.
    if (choice.impossible || choice.vertex == -1) {
        ++result_.stats.backtracks;
        return false;
    }

    std::vector<std::vector<int>> combinations;
    std::vector<int> current;
    EnumerateCombinations(choice.availableEdges, choice.need, 0, &current,
                          &combinations);

    for (const std::vector<int>& combination : combinations) {
        // Any valid layout is sufficient, so the first success ends the search.
        if (TryCombination(choice, combination, depth)) {
            return true;
        }
    }

    ++result_.stats.backtracks;
    return false;
}

// Try one local completion. Its edges are selected, and the chosen connector's
// other unknown incident edges are rejected. All changes occur after checkpoint
// and are rolled back together if the branch fails.
bool Solver::TryCombination(const BranchChoice& choice,
                            const std::vector<int>& combination, int depth) {
    // State at checkpoint belongs to the parent and is restored on failure.
    const std::size_t checkpoint = trail_.size();

    for (int edgeId : combination) {
        if (!AssignEdge(edgeId, kSelected)) {
            // A conflicting selected edge invalidates the entire subset.
            Rollback(checkpoint);
            return false;
        }
    }

    // The chosen connector is now filled, so every other unknown incident
    // candidate must be rejected.
    for (int edgeId : model_.incidentEdges[choice.vertex]) {
        if (edgeStates_[edgeId] == kUnknown &&
            !AssignEdge(edgeId, kRejected)) {
            Rollback(checkpoint);
            return false;
        }
    }

    if (Search(depth + 1)) {
        // Preserve selected states for MaterializeDirections on success.
        return true;
    }

    // Restore the parent state before trying the next completion subset.
    Rollback(checkpoint);
    return false;
}

// Rollback undoes every state transition after checkpoint in reverse order.
// Selected edges also decrement their endpoint degrees; all decided edges add
// one back to the unknown counts of both endpoints.
// The queue is cleared after rollback. Re-enqueuing inherited events would make
// the parent process a child's stale transitions, whereas the next child will
// enqueue whatever its own assignments require.
void Solver::Rollback(std::size_t checkpoint) {
    // Reverse order is required because state changes form a stack.
    while (trail_.size() > checkpoint) {
        const TrailEntry entry = trail_.back();
        trail_.pop_back();

        const CandidateEdge& edge = model_.edges[entry.edgeId];
        if (edgeStates_[entry.edgeId] == kSelected) {
            --selectedDegree_[edge.u];
            --selectedDegree_[edge.v];
        }
        // The edge returns to unknown, so both endpoint counters increase.
        ++unknownDegree_[edge.u];
        ++unknownDegree_[edge.v];
        edgeStates_[entry.edgeId] = entry.previousState;
    }

    // The parent must not inherit queued events from the failed child.
    pendingVertices_.clear();
    pendingHead_ = 0;
}

// Enumerate every need-sized subset of availableEdges. For example,
// [2,5,7] with need=2 produces [2,5], [2,7], and [5,7]. At most four
// candidates are available, so a simple recursive enumeration is sufficient.
void Solver::EnumerateCombinations(
    const std::vector<int>& availableEdges, int need, int start,
    std::vector<int>* current,
    std::vector<std::vector<int>>* combinations) const {
    // Store a complete subset.
    if (static_cast<int>(current->size()) == need) {
        combinations->push_back(*current);
        return;
    }

    // Prune when fewer elements remain than the subset still needs.
    const int remaining =
        need - static_cast<int>(current->size());
    const int slots = static_cast<int>(availableEdges.size()) - start;
    if (remaining > slots) {
        return;
    }

    for (int i = start;
         i <= static_cast<int>(availableEdges.size()) - remaining; ++i) {
        // Advance the start index to generate each unordered subset once.
        current->push_back(availableEdges[i]);
        EnumerateCombinations(availableEdges, need, i + 1, current,
                              combinations);
        current->pop_back();
    }
}

// Convert selected candidate edges to the four direction flags per connector.
// Each selected edge sets one reciprocal bit at each endpoint; rejected and
// unknown candidates leave their direction slots at zero.
void Solver::MaterializeDirections() {
    // Start with no incident fences.
    result_.directions.assign(
        model_.vertexCount, std::array<int, 4>{0, 0, 0, 0});

    // A selected candidate contributes one bit to each endpoint.
    for (int edgeId = 0; edgeId < model_.CandidateEdgeCount(); ++edgeId) {
        if (edgeStates_[edgeId] != kSelected) {
            continue;
        }
        const CandidateEdge& edge = model_.edges[edgeId];
        result_.directions[edge.u][edge.directionAtU] = 1;
        result_.directions[edge.v][edge.directionAtV] = 1;
    }
}

}  // namespace recover
