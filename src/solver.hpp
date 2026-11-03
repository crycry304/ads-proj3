#pragma once

#include "model.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace recover {

// 搜索统计用于比较后续剪枝版本的收益。
// nodes 表示进入过一次递归的搜索节点数；
// backtracks 表示一个局部组合失败并撤销的次数；
// maximumDepth 表示递归访问到的最大连接器层数。
struct SearchStats {
    // 每次进入 Search 时为 nodes 加一，即使该节点立即失败也算访问过。
    std::uint64_t nodes = 0;

    // 只有当前节点的所有局部组合都失败后才计入一次 backtrack。
    std::uint64_t backtracks = 0;

    // 从事件队列取出并重新检查的连接器次数。
    // 同一个连接器可能因多次状态变化而多次入队。
    std::uint64_t propagationEvents = 0;

    // 传播过程中由“已经满足度数”规则强制拒绝的边数。
    std::uint64_t forcedRejections = 0;

    // 传播过程中由“剩余边必须全选”规则强制选择的边数。
    std::uint64_t forcedSelections = 0;

    // 因为与已选边在空白格交叉而被自动拒绝的边数。
    std::uint64_t conflictRejections = 0;

    // 统计实际递归深度，用于后续比较固定顺序和 MRV。
    int maximumDepth = 0;
};

// found=true 表示找到了方向数组；
// found=false 表示完整搜索结束后没有合法布局。
struct SolveResult {
    // found 是搜索结论：false 表示完整搜索后确实没有解。
    bool found = false;

    // 找到解时按内部顶点编号保存四方向。
    std::vector<std::array<int, 4>> directions;

    // 即使无解，这些统计也可用于报告和性能比较。
    SearchStats stats;
};

// 当前求解器采用完备回溯、事件队列传播和 MRV 变量排序。
// Git 历史中的无传播版本用于比较搜索节点数。
//
// Solver 对象的生命周期只覆盖一次 Solve 调用。
// 当前不设计“复用上一次部分状态”的接口，
// 以避免不同测试用例之间的旧状态污染。
// 如果需要重复求解，应新建 Solver 或用新 Model 重新构造。
class Solver {
public:
    // Solver 只读取 Model，不修改静态候选图。
    explicit Solver(const Model& model);

    // 返回首个合法解或完整的无解结论。
    SolveResult Solve();

private:
    // 下面的私有接口共同维护三态回滚和局部组合探索。
    // 公共调用者只能看到最终 SolveResult，
    // 不应依赖搜索过程中的边编号或修改顺序。
    // UNKNOWN：当前分支尚未决定；
    // REJECTED：当前分支确定不选；
    // SELECTED：当前分支确定选择。
    enum EdgeState : std::uint8_t {
        kUnknown = 0,
        kRejected = 1,
        kSelected = 2,
    };

    // 回溯栈只记录一次 UNKNOWN -> REJECTED/SELECTED 的变化。
    struct TrailEntry {
        // edgeId 是被修改的候选边。
        int edgeId = -1;

        // previousState 在回溯时恢复；当前每条边只会从 UNKNOWN 进入确定状态。
        EdgeState previousState = kUnknown;
    };

    // 一个分支需要决定某个连接器的完整局部选择。
    struct BranchChoice {
        // vertex 是本次局部选择的连接器。
        int vertex = -1;

        // need 是该连接器还缺少的度数。
        int need = 0;

        // availableEdges 只包含当前仍为 UNKNOWN 的关联候选边。
        std::vector<int> availableEdges;

        // 当前连接器在现有状态下有多少个局部组合。
        // 形式为 C(availableEdges.size(), need)；
        // 该值越小，MRV 越优先选择这个连接器。
        int combinationCount = 0;

        // 所有未知关联边的冲突次数之和。
        // 只在 combinationCount 相同时作为第二关键字。
        int conflictWeight = 0;
    };

    bool AssignEdge(int edgeId, EdgeState newState);
    bool HasSelectedConflict(int edgeId) const;
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

    // edgeStates_ 的长度固定等于候选边数，下标就是全局边编号。
    std::vector<EdgeState> edgeStates_;

    // selectedDegree_ 的长度固定等于连接器数，只统计 SELECTED。
    std::vector<int> selectedDegree_;

    // unknownDegree_[v] 表示 v 的关联候选边中仍为 UNKNOWN 的数量。
    // selected + unknown < required 时可以立即判定当前分支失败。
    std::vector<int> unknownDegree_;

    // 增量传播的事件队列。
    // 某个端点状态变化后将连接器放入 pendingVertices_，
    // 直到队列消费完毕，所有可推导的强制状态都已处理。
    std::vector<int> pendingVertices_;
    std::size_t pendingHead_ = 0;

    // trail_ 是当前递归路径上的增量修改记录。
    std::vector<TrailEntry> trail_;

    // result_ 同时保存答案和搜索统计，随递归过程累积。
    SolveResult result_;
};

}  // namespace recover
