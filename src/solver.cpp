#include "solver.hpp"

#include <algorithm>

namespace recover {

// 第一版求解器的核心思想：
// 每次选一个尚未满足的连接器，一次性决定它还需要选择哪些未知候选边。
// 因为目标度数最多为 4，单个连接器的局部组合最多 C(4,2)=6 种。
// 选定组合后，该连接器的其余候选边必须拒绝，否则最终度数会超过要求。
//
// 这个版本暂不做事件队列传播，也不根据冲突提前拒绝整批边。
// 它仍然通过以下方式保证正确：
// - 每次选择都检查是否与已选边交叉；
// - 拒绝同一连接器未进入组合的未知边；
// - 所有局部选择都被穷举，找到首个解立即返回。
//
// 当前状态的不变量：
// - edgeStates_[e] 非 UNKNOWN 后，只能由 Rollback 恢复；
// - selectedDegree_[v] 等于所有 SELECTED 关联边的数量；
// - selectedDegree_[v] 永远不会大于 requiredDegree[v]；
// - 已处理连接器的所有未知关联边都已经被拒绝；
// - 任意两条同时 SELECTED 的候选边都没有冲突。
//
// 分支完备性的关键：
// 假设某个未满足连接器最终还需要 r 条边。
// 算法枚举它当前所有未知关联边中恰好大小为 r 的每个子集。
// 任何合法完成方案都必须选择其中一个子集，因此不会漏解。
// 选定一个子集后，其余未知关联边必须拒绝，所以下一次递归只处理更少选择。
//
// 这一版为什么不直接剪掉全部冲突边：
// 冲突的提前拒绝属于传播优化，会把多个状态修改连在一起。
// 基线先保持每次只处理当前连接器的局部选择，
// 便于用规模很小的暴力枚举验证搜索逻辑本身没有错。
//
// 对当前回溯的正确性证明可以拆成两部分：
// 1. 每次局部组合都逐项检查冲突和端点度数，若报告成功则一定是合法解；
// 2. 一个连接器的任一合法完整方案都必须从它的某个 need 子集开始，
//    而算法枚举了全部子集，所以失败分支不会漏掉潜在解。
//
// 这个基线暂时可能很慢，但它是后续所有优化的参照物。
// 增量传播和 MRV 只能减少访问节点，不能改变这个语义结论。
// 如果优化版本与基线结论不同，优先检查优化版本的剪枝是否过强。
//
// 当前搜索树中每个节点对应：
// - 已经确定的若干条候选边；
// - 已被拒绝的若干条候选边；
// - 尚未到达目标度数的连接器集合。
// 一次派生的子节点数量不超过 6；
// 每次失败恢复父节点状态，因此不同兄弟分支互不影响。

// 构造阶段只初始化边状态和每个连接器当前的已选度数。
// 这里不构建候选边，因为 Model 已经在 BuildModel 中完成静态构造。
Solver::Solver(const Model& model)
    : model_(model),
      edgeStates_(model.CandidateEdgeCount(), kUnknown),
      selectedDegree_(model.vertexCount, 0) {}

// Solve 是唯一公开入口。
// 它先处理静态模型已经确定的无解，再启动深度优先搜索。
// 搜索一旦成功就不再探索后续兄弟分支，满足题目“任意解”要求。
SolveResult Solver::Solve() {
    // 候选模型的必要条件已经失败时，不需要进入搜索。
    if (!model_.structurallyPossible) {
        return result_;
    }

    // 空模型没有连接器，Search(0) 会直接认为已经完成。
    if (Search(0)) {
        // 搜索只维护边的选择状态；
        // 找到解后统一转换成每个连接器的四方向数组。
        result_.found = true;
        MaterializeDirections();
    }
    // 无论成功还是失败都保留统计信息，便于报告比较搜索规模。
    return result_;
}

// AssignEdge 是当前版本唯一的状态修改入口。
//
// newState 只允许 REJECTED 或 SELECTED。
// 如果边已经是同一状态，重复赋值视为成功；
// 如果边已是相反状态，说明当前分支产生矛盾并返回 false。
//
// 变量命名约定：
// - edgeId 是全局候选边编号；
// - edge.u、edge.v 是两个连接器顶点编号；
// - selectedDegree_ 只统计 SELECTED，不统计 UNKNOWN 或 REJECTED。
bool Solver::AssignEdge(int edgeId, EdgeState newState) {
    // 三态一旦确定，在当前分支中不能直接被改写。
    if (edgeStates_[edgeId] != kUnknown) {
        return edgeStates_[edgeId] == newState;
    }

    // 先记录旧状态再修改，Rollback 可以精确撤销这一次赋值。
    trail_.push_back({edgeId, edgeStates_[edgeId]});
    edgeStates_[edgeId] = newState;

    if (newState == kSelected) {
        // 选边会同时增加两个端点的度数，这正是无向栅栏的含义。
        const CandidateEdge& edge = model_.edges[edgeId];
        ++selectedDegree_[edge.u];
        ++selectedDegree_[edge.v];

        // 选边前检查所有已经选中的冲突边。
        // 当前版本不顺手把冲突边标成 REJECTED，留到后续传播版本优化。
        if (HasSelectedConflict(edgeId)) {
            // 调用者会回滚 checkpoint，因此这里不需要自行恢复状态。
            return false;
        }
    }
    return true;
}

// 检查某条候选边是否与当前已选边冲突。
// conflictEdges[edgeId] 已经包含所有候选冲突边，
// 因此这里不需要扫描全部水平/竖直边。
bool Solver::HasSelectedConflict(int edgeId) const {
    // conflictEdges 是对称邻接表，只需检查当前边对应的一项。
    for (int other : model_.conflictEdges[edgeId]) {
        if (edgeStates_[other] == kSelected) {
            return true;
        }
    }
    return false;
}

// 完成条件不是“候选边都处理完”，而是“每个连接器度数准确”。
// 未选择的候选边可以是 UNKNOWN 或 REJECTED；
// 只要其两端连接器都已满足度数，答案就已经有效。
bool Solver::IsComplete() const {
    // 只有所有连接器都精确达到输入度数，答案才完成。
    for (int v = 0; v < model_.vertexCount; ++v) {
        if (selectedDegree_[v] != model_.requiredDegree[v]) {
            return false;
        }
    }
    return true;
}

// Search 返回 true 表示当前状态已经找到一个完整合法答案。
// Search 返回 false 表示当前状态的所有局部组合都已穷尽且全部失败。
//
// depth 只用于统计递归深度，不参与约束判断。
// 当前选择顺序固定为最小未满足顶点编号，
// 后续 MRV 版本会替换这一选择策略，但会保留相同的递归语义。
bool Solver::Search(int depth) {
    ++result_.stats.nodes;
    result_.stats.maximumDepth =
        std::max(result_.stats.maximumDepth, depth);

    if (IsComplete()) {
        return true;
    }

    // 基线版本固定选择编号最小的未满足连接器。
    // 这是确定性顺序，便于后续 MRV 版本做搜索节点对比。
    BranchChoice choice;
    for (int v = 0; v < model_.vertexCount; ++v) {
        if (selectedDegree_[v] >= model_.requiredDegree[v]) {
            continue;
        }

        choice.vertex = v;
        // need 表示这个连接器还差多少条边，而不是最终总度数。
        // 例如 requiredDegree=3、已选 1 条边时，need=2。
        choice.need = model_.requiredDegree[v] - selectedDegree_[v];
        for (int edgeId : model_.incidentEdges[v]) {
            if (edgeStates_[edgeId] == kUnknown) {
                choice.availableEdges.push_back(edgeId);
            }
        }
        break;
    }

    // 找不到未满足连接器但 IsComplete 为假，说明内部状态异常。
    if (choice.vertex == -1 ||
        static_cast<int>(choice.availableEdges.size()) < choice.need) {
        ++result_.stats.backtracks;
        return false;
    }

    std::vector<std::vector<int>> combinations;
    std::vector<int> current;
    EnumerateCombinations(choice.availableEdges, choice.need, 0, &current,
                          &combinations);

    for (const std::vector<int>& combination : combinations) {
        // 第一个成功组合直接结束整个搜索。
        // 题目接受任意解，无需继续寻找其他组合。
        if (TryCombination(choice, combination, depth)) {
            return true;
        }
    }

    ++result_.stats.backtracks;
    return false;
}

// 尝试一个完整的局部选择 combination。
//
// 组合中的边会被 SELECTED；
// 同一连接器其余仍为 UNKNOWN 的边会被 REJECTED。
// 这些修改都在 checkpoint 之后，无论成功还是失败都使用同一套撤销规则。
bool Solver::TryCombination(const BranchChoice& choice,
                            const std::vector<int>& combination, int depth) {
    // checkpoint 之前的边状态属于父分支，失败时必须恢复到这个位置。
    const std::size_t checkpoint = trail_.size();

    for (int edgeId : combination) {
        if (!AssignEdge(edgeId, kSelected)) {
            // 一旦某条选中边与已经选中的边冲突，整个组合不可用。
            Rollback(checkpoint);
            return false;
        }
    }

    // 该连接器的度数已经由 combination 填满，
    // 因此同一个连接器的其他未知边都必须拒绝。
    for (int edgeId : model_.incidentEdges[choice.vertex]) {
        if (edgeStates_[edgeId] == kUnknown &&
            !AssignEdge(edgeId, kRejected)) {
            Rollback(checkpoint);
            return false;
        }
    }

    if (Search(depth + 1)) {
        // 成功后不回滚，保留最终答案所需的边状态。
        return true;
    }

    // 子搜索失败时撤销本次局部组合，再尝试下一个组合。
    Rollback(checkpoint);
    return false;
}

// Rollback 撤销 checkpoint 之后的所有边的状态变化。
//
// 状态按修改的逆序恢复，因此同一个边即使将来在传播版本中
// 经历多次变化，也能逐项回到父分支的状态。
// 对 SELECTED 边还要减回两个端点的度数。
void Solver::Rollback(std::size_t checkpoint) {
    // 逆序撤销状态变化，并同步减回已选度数。
    while (trail_.size() > checkpoint) {
        const TrailEntry entry = trail_.back();
        trail_.pop_back();

        if (edgeStates_[entry.edgeId] == kSelected) {
            const CandidateEdge& edge = model_.edges[entry.edgeId];
            --selectedDegree_[edge.u];
            --selectedDegree_[edge.v];
        }
        edgeStates_[entry.edgeId] = entry.previousState;
    }
}

// 生成 availableEdges 中所有大小恰好为 need 的组合。
//
// 例子：availableEdges=[2,5,7]，need=2 时生成
// [2,5]、[2,7]、[5,7] 三个组合。
// 每个连接器最多四条候选边，所以组合总数不超过 6，
// 不需要为一般 k/n 编写更复杂的数据结构。
void Solver::EnumerateCombinations(
    const std::vector<int>& availableEdges, int need, int start,
    std::vector<int>* current,
    std::vector<std::vector<int>>* combinations) const {
    // 当当前组合长度达到 need 时保存一份结果。
    if (static_cast<int>(current->size()) == need) {
        combinations->push_back(*current);
        return;
    }

    // 即使把剩余元素全部放进来仍不足 need，就提前停止该递归分支。
    const int remaining =
        need - static_cast<int>(current->size());
    const int slots = static_cast<int>(availableEdges.size()) - start;
    if (remaining > slots) {
        return;
    }

    for (int i = start;
         i <= static_cast<int>(availableEdges.size()) - remaining; ++i) {
        // 选择第 i 条边后，后续递归只能从 i+1 开始，
        // 避免同一组合因不同顺序被重复枚举。
        current->push_back(availableEdges[i]);
        EnumerateCombinations(availableEdges, need, i + 1, current,
                              combinations);
        current->pop_back();
    }
}

// 搜索完成后的最后一步：把边状态翻译成题目需要的数据。
//
// 候选边 e 对应：
// - directions[e.u][e.directionAtU] = 1；
// - directions[e.v][e.directionAtV] = 1。
// REJECTED 和 UNKNOWN 都保持 0。
void Solver::MaterializeDirections() {
    // 初始时所有连接器的四个方向都是 0。
    result_.directions.assign(
        model_.vertexCount, std::array<int, 4>{0, 0, 0, 0});

    // 每条 SELECTED 候选边会让两个端点各有一个方向变成 1。
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
