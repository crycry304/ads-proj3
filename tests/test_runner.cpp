#include "io.hpp"
#include "model.hpp"
#include "solver.hpp"
#include "validator.hpp"

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// 测试程序直接链接生产模块，不复制一套“测试专用算法”。
// 因此这里发现的问题就是实际源码中的问题。
//
// 当前测试分四层：
// 1. 输入解析：合法、越界、额外 token；
// 2. 候选模型：顶点数、候选边数、冲突数、静态可行性；
// 3. 答案格式：题目两个合法输出、打印往返、错误度数；
// 4. 特殊边界：非法交叉和全零空设计。
//
// 两套独立的解存在性判断：
// - BruteForceExists 枚举候选边子集，作为小规模测试真值；
// - Solver 使用完整回溯、传播和 MRV。
// 只有两者在所有可承受的输入上结论一致，优化后的求解器才可信。
//
// 验证不依赖具体答案文本，因为题目允许多解。
// 对每个找到的解，只检查它是否满足输入规定的全部约束。
// 无解样例则单独检查求解器是否完整搜索后返回 false。
//
// 随机测试全部使用固定 seed，保证 CI 和本地测试可重复。
// 若测试失败，context 会指出具体测试组和序号。

// 测试失败不会立即终止进程，而是累计失败数，
// 这样一次运行可以同时暴露多个约束或格式问题。

// 所有测试共享失败计数，最后统一决定进程退出码。
int failures = 0;

// 条件不成立时打印具体测试目的，便于直接定位失败模块。
void Expect(bool condition, const std::string& message) {
    if (!condition) {
        // 某个断言失败后继续运行，方便一次看到多个问题。
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// 把字符串输入转换成完整模型，减少测试中的重复样板代码。
recover::Model BuildFromText(const std::string& text) {
    std::istringstream input(text);
    recover::Grid grid;
    std::string error;
    Expect(recover::ReadSingleCase(&input, &grid, &error), error);

    // 输入解析和模型构造是两个独立阶段。
    recover::Model model;
    Expect(recover::BuildModel(grid, &model, &error), error);
    return model;
}

// 调用独立的输出验证器检查候选答案。
recover::ValidationResult ValidateText(const recover::Model& model,
                                       const std::string& text) {
    std::istringstream output(text);
    return recover::ValidateOutput(model, &output);
}

// 小规模暴力搜索只枚举候选边子集。
// 它不依赖正式 Solver 的递归结构，因此适合作为独立真值来源。
bool BruteForceExists(const recover::Model& model) {
    const int edgeCount = model.CandidateEdgeCount();
    if (edgeCount > 20) {
        return false;
    }

    const std::uint64_t limit = std::uint64_t{1} << edgeCount;
    const int targetEdges = model.totalRequiredDegree / 2;

    for (std::uint64_t mask = 0; mask < limit; ++mask) {
        // 只需要枚举恰好选择一半总度数的子集。
        if (static_cast<int>(std::bitset<64>(mask).count()) != targetEdges) {
            continue;
        }

        std::vector<int> degree(model.vertexCount, 0);
        bool valid = true;
        for (int edgeId = 0; edgeId < edgeCount && valid; ++edgeId) {
            if ((mask & (std::uint64_t{1} << edgeId)) == 0) {
                continue;
            }

            const recover::CandidateEdge& edge = model.edges[edgeId];
            ++degree[edge.u];
            ++degree[edge.v];
            if (degree[edge.u] > model.requiredDegree[edge.u] ||
                degree[edge.v] > model.requiredDegree[edge.v]) {
                valid = false;
                break;
            }

            for (int other : model.conflictEdges[edgeId]) {
                if ((mask & (std::uint64_t{1} << other)) != 0) {
                    valid = false;
                    break;
                }
            }
        }

        if (!valid) {
            continue;
        }
        // 掩码中的边都不冲突后，再统一检查所有顶点的精确度数。
        for (int v = 0; v < model.vertexCount; ++v) {
            if (degree[v] != model.requiredDegree[v]) {
                valid = false;
                break;
            }
        }
        if (valid) {
            // 暴力枚举只需要判断是否存在，找到第一个就立即返回。
            return true;
        }
    }
    return false;
}

void TestSolverAndValidation(const std::vector<std::vector<int>>& grid,
                             bool expectedFound) {
    recover::Model model;
    std::string error;
    Expect(recover::BuildModel(grid, &model, &error), error);

    recover::Solver solver(model);
    const recover::SolveResult result = solver.Solve();
    // 有解/无解结论先与样例预期比较。
    Expect(result.found == expectedFound, "solver solvability mismatch");
    if (!result.found) {
        // 无解样例没有方向数组可供验证，只比较预期结论。
        return;
    }

    // 有解时用正式 IO 生成文本，再走独立 Validator。
    std::ostringstream output;
    recover::PrintDirections(model, result.directions, &output);
    Expect(ValidateText(model, output.str()).status ==
               recover::ValidationStatus::kValid,
           "solver output must pass independent validation");
}

// 对拍入口依次完成建模、暴力真值计算、正式求解和最终答案验证。
// context 用于在失败时指出具体尺寸和序号，便于直接复现。
void CheckDifferentialCase(const std::vector<std::vector<int>>& grid,
                           const std::string& context) {
    // 对拍函数先从输入重新构建模型，再用独立候选边子集枚举判断真值。
    recover::Model model;
    std::string error;
    Expect(recover::BuildModel(grid, &model, &error),
           context + ": " + error);

    // expected 完全由 BruteForceExists 决定，不读取 Solver 的搜索痕迹。
    const bool expected = BruteForceExists(model);
    recover::Solver solver(model);
    const recover::SolveResult result = solver.Solve();
    Expect(result.found == expected,
           context + ": solver disagrees with brute force");
    if (!result.found) {
        return;
    }

    std::ostringstream output;
    recover::PrintDirections(model, result.directions, &output);
    const recover::ValidationResult validation =
        ValidateText(model, output.str());
    Expect(validation.status == recover::ValidationStatus::kValid,
           context + ": " + validation.message);
}

// 完整穷举 2x2 和 2x3 的所有度数矩阵。
// 这比随机抽样更严格：输入空间中的每个点都会被检查一次。
void TestExhaustiveSmallGrids() {
    // 每个格子独立取 0..4。2x3 只有 5^6=15625 种输入，
    // 可以直接与候选边子集暴力枚举逐项对拍。
    for (const auto& dimensions : std::vector<std::pair<int, int>>{
             {2, 2}, {2, 3}}) {
        const int rows = dimensions.first;
        const int cols = dimensions.second;
        // 使用五进制编码逐项生成所有度数矩阵。
        const int cells = rows * cols;
        // 计算 5^(rows*cols)，作为所有度数矩阵的五进制编号上界。
        int total = 1;
        for (int i = 0; i < cells; ++i) {
            total *= 5;
        }

        for (int code = 0; code < total; ++code) {
            // 把当前编号拆成每个格子的 0..4 度数。
            int value = code;
            std::vector<std::vector<int>> grid(
                rows, std::vector<int>(cols, 0));
            // 五进制数字的每一位对应一个格子的度数 0..4。
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    grid[r][c] = value % 5;
                    value /= 5;
                }
            }
            CheckDifferentialCase(
                grid, "exhaustive " + std::to_string(rows) + "x" +
                          std::to_string(cols) + " code " +
                          std::to_string(code));
            if (failures > 5) {
                // 同类错误已经很多时提前结束，保留可读的测试输出。
                return;
            }
        }
    }
}

// 3x3 和 3x4 的候选边最多约 14 条，
// 因此仍可通过候选边子集暴力枚举得到可靠真值。
void TestRandomThreeByThreeAndThreeByFour() {
    // 3×4 最多约 14 条候选边，仍可承受 2^14 子集枚举。
    std::mt19937 generator(20261103U);
    std::uniform_int_distribution<int> rowsDistribution(3, 3);
    std::uniform_int_distribution<int> colsDistribution(3, 4);
    std::bernoulli_distribution nonZero(0.45);
    std::uniform_int_distribution<int> degreeDistribution(1, 4);

    for (int test = 0; test < 3000; ++test) {
        const int rows = rowsDistribution(generator);
        const int cols = colsDistribution(generator);
        std::vector<std::vector<int>> grid(
            rows, std::vector<int>(cols, 0));
        // 每格先决定是否出现连接器，再生成 1..4 的目标度数。
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (nonZero(generator)) {
                    grid[r][c] = degreeDistribution(generator);
                }
            }
        }
        CheckDifferentialCase(grid, "random differential " +
                                         std::to_string(test));
        if (failures > 5) {
            return;
        }
    }
}

// 先构造一个无冲突的合法布局，再由它反推出每个格子的度数。
// 因此这些输入按构造必然有解，Solver 不应输出 No Solution。
void TestPlantedFeasibleLayouts() {
    // 先在正数掩码上随机选择一个无冲突边集合，
    // 再由该集合反算每个连接器的度数。
    // 因此构造出的输入一定存在解，可用于检查“误报无解”。
    std::mt19937 generator(20261104U);
    std::uniform_int_distribution<int> sizeDistribution(3, 6);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    for (int test = 0; test < 200; ++test) {
        // 随机选择矩阵尺寸和正数位置密度。
        const int rows = sizeDistribution(generator);
        const int cols = sizeDistribution(generator);
        const double density = unit(generator);

        std::vector<std::vector<int>> mask(
            rows, std::vector<int>(cols, 0));
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                mask[r][c] = unit(generator) < density ? 1 : 0;
            }
        }

        // 先用占位度数 1 建立候选图，只借助它取得边长和冲突关系。
        recover::Model base;
        std::string error;
        Expect(recover::BuildModel(mask, &base, &error), error);
        if (base.CandidateEdgeCount() == 0) {
            // 没有候选边就没有可供种植的合法栅栏，跳过空案例。
            continue;
        }

        // 打乱候选边，随后贪心选择无冲突子集作为隐藏的可行设计方案。
        std::vector<int> order(base.CandidateEdgeCount());
        for (int i = 0; i < base.CandidateEdgeCount(); ++i) {
            order[i] = i;
        }
        std::shuffle(order.begin(), order.end(), generator);

        std::vector<bool> selected(base.CandidateEdgeCount(), false);
        std::vector<int> vertexDegree(base.vertexCount, 0);
        // 隐藏方案中每条选边给两端各增加 1 度。
        for (int edgeId : order) {
            // 约 55% 的候选边进入隐藏方案；冲突边则直接跳过。
            if (unit(generator) > 0.55) {
                continue;
            }
            bool conflicts = false;
            for (int other : base.conflictEdges[edgeId]) {
                if (selected[other]) {
                    conflicts = true;
                    break;
                }
            }
            if (conflicts) {
                // 与已选边交叉的候选边不能加入隐藏方案。
                continue;
            }
            selected[edgeId] = true;
            const recover::CandidateEdge& edge = base.edges[edgeId];
            ++vertexDegree[edge.u];
            ++vertexDegree[edge.v];
        }

        // 将隐藏方案的端点度数写回矩阵，零度位置改回 0。
        std::vector<std::vector<int>> grid(
            rows, std::vector<int>(cols, 0));
        bool any = false;
        for (int v = 0; v < base.vertexCount; ++v) {
            grid[base.vertexRow[v]][base.vertexCol[v]] = vertexDegree[v];
            any = any || vertexDegree[v] > 0;
        }
        if (any) {
            // 隐藏方案至少含一条边，因此生成输入必然存在解。
            TestSolverAndValidation(grid, true);
        }
    }
}

// 覆盖题目最容易误判的几何和计数边界。
// 每个 TestSolverAndValidation 的第二个参数是预期是否存在解。
void TestRequiredEdgeCases() {
    // 单连接器没有另一端，不能形成栅栏。
    TestSolverAndValidation({{1, 0}, {0, 0}}, false);

    // 相邻的左右两点各需要一条边，必须直通。
    TestSolverAndValidation({{1, 1}, {0, 0}}, true);

    // 相邻的上下两点各需要一条边。
    TestSolverAndValidation({{1, 0}, {1, 0}}, true);

    // 跨越中间零位置的一条长水平边。
    TestSolverAndValidation({{1, 0, 0, 1}, {0, 0, 0, 0}}, true);

    // 中心连接四个方向，形成四向分叉。
    // 四条臂各自只要求 1 度，因此唯一完成方式是全部与中心相连。
    TestSolverAndValidation(
        {{0, 1, 0}, {1, 4, 1}, {0, 1, 0}}, true);

    // 只保留三个方向的 T 形连接器。
    // 缺少下侧候选边时，中心度数 3 正好对应上、左、右三条边。
    TestSolverAndValidation(
        {{0, 1, 0}, {1, 3, 1}, {0, 0, 0}}, true);

    // 两个互不相连的合法分量。
    // 题目不要求所有连接器属于同一个栅栏，因此分开的两条边也合法。
    TestSolverAndValidation(
        {{1, 1, 0, 1, 1}, {0, 0, 0, 0, 0}}, true);

    // 边界格只有两个方向，需求 4 条边必然不可能。
    TestSolverAndValidation({{4, 0}, {0, 0}}, false);

    // 总度数为奇数，候选图再好也无法两两配对。
    // 每条边贡献 2 度，因此三条需求边无法闭合成合法选边集合。
    TestSolverAndValidation({{1, 1, 0}, {1, 0, 0}}, false);
}

// 三个官方样例作为固定回归用例。
// 答案可能不唯一，所以比较的是合法性而不是固定的输出字符串。
void TestSolverSamples() {
    // Sample 1 是普通可行实例，用于确认搜索能找到完整布局。
    TestSolverAndValidation(
        {
            {0, 0, 2, 0, 0, 2},
            {1, 3, 3, 1, 0, 0},
            {2, 2, 1, 0, 0, 2},
            {0, 0, 0, 0, 0, 1},
            {2, 0, 2, 0, 0, 2},
        },
        true);

    // Sample 2 的总度数为奇数，模型静态检查会直接拒绝。
    TestSolverAndValidation(
        {
            {0, 0, 2, 0, 0, 2},
            {1, 3, 3, 1, 0, 0},
            {2, 2, 1, 0, 0, 1},
            {0, 0, 0, 0, 0, 1},
            {2, 0, 2, 0, 0, 2},
        },
        false);

    // Sample 3 含跨空白的候选段和一个交叉冲突。
    TestSolverAndValidation(
        {
            {1, 1, 2, 0, 1},
            {1, 0, 3, 0, 2},
            {1, 2, 1, 0, 1},
        },
        true);
}

// 固定种子的随机差异测试，覆盖 2x2 到 3x4。
// 该组与后续 3x3/3x4 专项组互补。
void TestSolverAgainstBruteForce() {
    // 固定随机种子保证失败可以复现。
    std::mt19937 generator(20261102U);
    std::uniform_int_distribution<int> rows(2, 3);
    std::uniform_int_distribution<int> cols(2, 4);
    std::bernoulli_distribution nonZero(0.45);
    std::uniform_int_distribution<int> degree(1, 4);

    for (int test = 0; test < 1000; ++test) {
        const int n = rows(generator);
        const int m = cols(generator);
        std::vector<std::vector<int>> grid(n, std::vector<int>(m, 0));
        for (int r = 0; r < n; ++r) {
            for (int c = 0; c < m; ++c) {
                if (nonZero(generator)) {
                    grid[r][c] = degree(generator);
                }
            }
        }

        recover::Model model;
        std::string error;
        Expect(recover::BuildModel(grid, &model, &error), error);

        const bool expected = BruteForceExists(model);
        // 正式求解器与独立暴力枚举必须在“是否存在解”上完全一致。
        recover::Solver solver(model);
        const recover::SolveResult result = solver.Solve();
        // 这类测试同时覆盖有解和无解，不能只验证能成功构造的答案。
        Expect(result.found == expected,
               "random model disagrees with brute-force result");
        if (failures > 5) {
            // 连续出现多个不一致时提前停止，避免日志被同一类错误淹没。
            return;
        }
    }
}

// 强制传播专项测试不仅要求有解，
// 还要求统计中确实发生 queued propagation 和 forced selection。
void TestForcedPropagation() {
    // 上排度数为 1、2、1，下面一行全为零。
    // 两条水平候选边必须同时选择；选择第一条后，
    // 中间连接器只剩一条未知边，传播应自动强制选择第二条。
    recover::Model model = BuildFromText(
        "2 3\n"
        "1 2 1\n"
        "0 0 0\n");
    recover::Solver solver(model);
    const recover::SolveResult result = solver.Solve();

    Expect(result.found, "forced-selection fixture must be solvable");
    Expect(result.stats.propagationEvents > 0,
           "propagation queue must process changed endpoints");
    Expect(result.stats.forcedSelections > 0,
           "fixture must trigger forced selection");
}

// 冲突传播测试不只检查最终无解，
// 还检查统计中确实发生了“选中一条边后自动拒绝交叉边”。
// 这能防止冲突拒绝逻辑被意外删除，却仍因其他规则碰巧得到无解。
// 冲突专项测试检查选中边确实触发交叉边拒绝。
void TestConflictPropagation() {
    // 四个 1 位于十字端点，横竖两条候选长边在中心零位置冲突。
    // 选中第一条后，传播应立即拒绝另一条，并发现四个端点无法全部满足。
    recover::Model model = BuildFromText(
        "3 3\n"
        "0 1 0\n"
        "1 0 1\n"
        "0 1 0\n");
    recover::Solver solver(model);
    const recover::SolveResult result = solver.Solve();

    Expect(!result.found, "crossing fixture must be impossible");
    Expect(result.stats.conflictRejections > 0,
           "selected edge must reject its crossing edge");
}

// 解析测试覆盖三类输入行为：
// 1. 合法矩阵应被完整读入；
// 2. 单个度数越界应在进入建模前失败；
// 3. 单测试用例后出现额外数据应被拒绝。
//
// 这些检查的目的是把输入格式错误和真正的算法无解分开。
// 如果解析器误接受非法输入，后续模型可能出现越界或错误候选边；
// 如果解析器误拒绝合法输入，所有搜索测试都无法可靠进行。
void TestParser() {
    // 正常输入：应成功解析，并保持题目给出的二维规模。
    recover::Grid grid;
    std::string error;
    std::istringstream valid("2 3\n1 0 2\n0 3 0\n");
    Expect(recover::ReadSingleCase(&valid, &grid, &error), "valid input");
    Expect(grid.size() == 2 && grid[0].size() == 3, "grid dimensions");

    // 度数为 5 超出单个连接器最多四个方向，必须被解析器拒绝。
    std::istringstream badDegree("2 2\n1 2\n3 5\n");
    Expect(!recover::ReadSingleCase(&badDegree, &grid, &error),
           "degree 5 must be rejected");

    // 题目只有一个测试用例，末尾多出的整数应被拒绝。
    std::istringstream extraToken("2 2\n1 0\n0 0\n7\n");
    Expect(!recover::ReadSingleCase(&extraToken, &grid, &error),
           "extra input token must be rejected");
}

// 候选模型测试不调用求解器，只检查静态构造结果。
//
// Sample 3 的关键价值是跨空白位置的长边与交叉冲突；
// Sample 2 的关键价值是用度数奇偶性在搜索前直接判定不可行。
// 两项测试分别覆盖“模型必须足够表达问题”和“模型必须尽早排除无解”。
//
// 这里同时确认顶点、候选边和冲突对的数量，防止候选边漏建或重复建。
void TestCandidateModel() {
    // Sample 3 包含跨空白位置的长边，并有一组交叉冲突。
    const recover::Model sample3 = BuildFromText(
        "3 5\n"
        "1 1 2 0 1\n"
        "1 0 3 0 2\n"
        "1 2 1 0 1\n");
    // 11 非零格子产生了 11 个顶点。
    // 行候选段与列候选段合计 15 条，其中水平跨空白的边把冲突暴露出来。
    Expect(sample3.vertexCount == 11, "sample 3 connector count");
    // 11 个顶点在本样例中产生 15 条行/列候选段。
    Expect(sample3.CandidateEdgeCount() == 15, "sample 3 edge count");
    Expect(sample3.conflictPairCount == 1, "sample 3 conflict count");
    // 静态检查通过只表示必要条件成立，真正的解仍要由搜索器寻找。
    Expect(sample3.structurallyPossible, "sample 3 static feasibility");

    // Sample 2 的总度数为奇数，因此候选模型可直接判断不可行。
    const recover::Model sample2 = BuildFromText(
        "5 6\n"
        "0 0 2 0 0 2\n"
        "1 3 3 1 0 0\n"
        "2 2 1 0 0 1\n"
        "0 0 0 0 0 1\n"
        "2 0 2 0 0 2\n");
    // 25 是奇数；任何栅栏都会让两个端点各增加 1 度，
    // 所有选边贡献的总度数必然为偶数，因此可以在搜索前停止。
    Expect(sample2.totalRequiredDegree == 25,
           "sample 2 total requested degree");
    Expect(!sample2.structurallyPossible,
           "odd total degree must be rejected statically");
}

// 答案验证测试覆盖一个答案从文本到结构的完整检查过程。
//
// Sample 1 验证普通合法输出，Sample 3 验证另一个含长边和多解的输出。
// 打印往返测试确保 PrintDirections 不会改变坐标或方向顺序。
// 错误度数测试确认仅有格式正确还不够，必须满足输入规定的度数。
//
// 注意：No Solution 在本测试中只验证状态识别，
// 不把“输出这两个词”误当成已经证明了实例无解。
void TestOutputValidation() {
    // 使用题目给出的 Sample 1 输出验证完整合法路径。
    const recover::Model sample1 = BuildFromText(
        "5 6\n"
        "0 0 2 0 0 2\n"
        "1 3 3 1 0 0\n"
        "2 2 1 0 0 2\n"
        "0 0 0 0 0 1\n"
        "2 0 2 0 0 2\n");
    // 下方文本来自题目 Sample 1。
    // 它有 14 个连接器，每行六个数字：
    // 行号、列号、上、下、左、右。
    const recover::ValidationResult sample1Result = ValidateText(
        sample1,
        "1 3 0 1 0 1\n"
        "1 6 0 1 1 0\n"
        "2 1 0 0 0 1\n"
        "2 2 0 1 1 1\n"
        "2 3 1 0 1 1\n"
        "2 4 0 0 1 0\n"
        "3 1 0 1 0 1\n"
        "3 2 1 0 1 0\n"
        "3 3 0 0 0 1\n"
        "3 6 1 0 1 0\n"
        "4 6 0 1 0 0\n"
        "5 1 1 0 0 1\n"
        "5 3 0 0 1 1\n"
        "5 6 1 0 1 0\n");
    Expect(sample1Result.status == recover::ValidationStatus::kValid,
           "sample 1 output must validate");

    // 把验证器解析出的方向重新输出并再次验证，测试打印格式的往返一致。
    // 这一步能同时覆盖 PrintDirections 的坐标转换和行优先顺序。
    std::ostringstream roundTrip;
    recover::PrintDirections(sample1, sample1Result.directions, &roundTrip);
    Expect(ValidateText(sample1, roundTrip.str()).status ==
               recover::ValidationStatus::kValid,
           "print/validate round trip");

    // Sample 3 有多个解，这里只验证题目给出的第一个解。
    const recover::Model sample3 = BuildFromText(
        "3 5\n"
        "1 1 2 0 1\n"
        "1 0 3 0 2\n"
        "1 2 1 0 1\n");
    const recover::ValidationResult sample3Result = ValidateText(
        sample3,
        "1 1 0 0 0 1\n"
        "1 2 0 0 1 0\n"
        "1 3 0 1 0 1\n"
        "1 5 0 0 1 0\n"
        "2 1 0 0 0 1\n"
        "2 3 1 0 1 1\n"
        "2 5 0 1 1 0\n"
        "3 1 0 0 0 1\n"
        "3 2 0 0 1 1\n"
        "3 3 0 0 1 0\n"
        "3 5 1 0 0 0\n");
    Expect(sample3Result.status == recover::ValidationStatus::kValid,
           "sample 3 output must validate");

    // No Solution 只是文本状态，是否真的无解要由求解器证明。
    Expect(ValidateText(sample3, "No Solution\n").status ==
               recover::ValidationStatus::kNoSolution,
           "No Solution marker");
    // 验证器无法从 NO 输出反推全局无解，因此这里只测试状态识别。

    // 将最后一个连接器的度数改错，验证器必须拒绝。
    // 该连接器原度数为 1，这里把六个数字全部置 0。
    std::string wrongDegree =
        "1 1 0 0 0 1\n"
        "1 2 0 0 1 0\n"
        "1 3 0 1 0 1\n"
        "1 5 0 0 1 0\n"
        "2 1 0 0 0 1\n"
        "2 3 1 0 1 1\n"
        "2 5 0 1 1 0\n"
        "3 1 0 0 0 1\n"
        "3 2 0 0 1 1\n"
        "3 3 0 0 1 0\n"
        "3 5 0 0 0 0\n";
    Expect(ValidateText(sample3, wrongDegree).status ==
               recover::ValidationStatus::kInvalid,
           "wrong degree must fail");
}

// 交叉测试刻意构造一个“只看端点和度数似乎完全正确”的答案：
// 四个端点的度数都是 1，两条候选边也都两端匹配。
// 唯一的错误是两条长边在中心零位置相交。
//
// 这个测试防止后续优化时把交叉约束漏掉，或只验证半条栅栏。
void TestIllegalCrossing() {
    // 四个连接器组成十字形的四个端点，中心位置保持为 0。
    // 两条候选长边都穿过中心，因此预处理阶段应生成一组冲突。
    // 若两条长边同时选中，它们会在中心非法交叉。
    const recover::Model crossing = BuildFromText(
        "3 3\n"
        "0 1 0\n"
        "1 0 1\n"
        "0 1 0\n");
    Expect(crossing.conflictPairCount == 1,
           "crossing model must contain one conflict");

    // 这个布局满足每个端点度数为 1，但违反交叉规则。
    const std::string layout =
        "1 2 0 1 0 0\n"
        "2 1 0 0 0 1\n"
        "2 3 0 0 1 0\n"
        "3 2 1 0 0 0\n";
    // 布局让上端向下、下端向上，同时让左端向右、右端向左。
    // 每条候选边两端一致，四个端点度数也都为 1；
    // 唯一的错误是两条边在中心的零位置交叉。
    Expect(ValidateText(crossing, layout).status ==
               recover::ValidationStatus::kInvalid,
           "fences crossing at an empty point must fail");
}

// 空设计是容易被遗漏的边界条件。
// 输入没有非零连接器时，不应输出任何连接器行，
// 也不应把空输出误报为格式错误或 No Solution。
void TestEmptyLayout() {
    // 没有任何连接器时，空输出就是合法的空设计。
    const recover::Model empty = BuildFromText(
        "2 2\n"
        "0 0\n"
        "0 0\n");
    const recover::ValidationResult result = ValidateText(empty, "");
    // 没有连接器时，四个方向数组长度为零，不需要输出任何行。
    // 空模型的方向数组也为空，验证器不应要求打印任何连接器行。
    Expect(result.status == recover::ValidationStatus::kValid,
           "all-zero grid accepts an empty layout");
}

}  // namespace

int main() {
    // 按输入解析、候选模型、输出验证和边界情况分组运行。
    TestParser();
    TestCandidateModel();
    TestOutputValidation();
    TestIllegalCrossing();
    TestEmptyLayout();
    TestSolverSamples();
    TestSolverAgainstBruteForce();
    TestForcedPropagation();
    TestConflictPropagation();
    TestExhaustiveSmallGrids();
    TestRandomThreeByThreeAndThreeByFour();
    TestPlantedFeasibleLayouts();
    TestRequiredEdgeCases();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All model and validation tests passed\n";
    return 0;
}
