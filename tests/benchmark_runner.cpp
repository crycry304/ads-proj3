#include "io.hpp"
#include "model.hpp"
#include "solver.hpp"
#include "validator.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// 该文件故意不包含生成随机数据的代码。
// 输入案例由 tools/generate_cases.py 独立生成，
// 这样同一批文件可以同时供三个历史算法版本测量。

// 性能程序只负责组织和输出测量数据，不复制求解逻辑。
// 每一行 CSV 对应一次完整的 Model 构建与 Solver 求解。

// 读取测试文件并重建候选模型。
// 这个函数同时用于不计时的预热和正式测量。
// path 可以包含空格，由操作系统参数传递，不参与 CSV 文件名解析。
bool LoadCase(const std::string& path, recover::Grid* grid,
              std::string* error) {
    std::ifstream input(path);
    if (!input) {
        *error = "cannot open " + path;
        return false;
    }
    return recover::ReadSingleCase(&input, grid, error);
}

// 运行一次完整求解，并在有解时使用独立验证器复查。
// 返回空字符串表示本次运行成功；否则返回具体错误。
// wall_ms 只测量 BuildModel 和 Solver::Solve，
// 不包括读入网格、后端字符串构造或 Validator 复查。
std::string RunOnce(const std::string& path, bool check_answer,
                    recover::SearchStats* stats, bool* found, double* wall_ms) {
    // 输入读取和文件打开通常在操作系统缓存中完成，
    // 计时从构建 Model 前开始，并覆盖 Solver::Solve。
    recover::Grid grid;
    std::string error;
    if (!LoadCase(path, &grid, &error)) {
        // 输入格式错误属于测试基础设施错误，不属于算法无解。
        return error;
    }

    // Model 构建和搜索都计入 wall_ms；
    // 后续若需要单独统计预处理，可以把计时起点移动到 BuildModel 之后。
    const auto start = std::chrono::steady_clock::now();
    recover::Model model;
    if (!recover::BuildModel(grid, &model, &error)) {
        // 建模失败同样直接返回错误，不能写成 found=0。
        return error;
    }
    recover::Solver solver(model);
    const recover::SolveResult result = solver.Solve();
    const auto finish = std::chrono::steady_clock::now();

    // 统计量从求解器直接复制，避免根据耗时或节点数做推测。
    *stats = result.stats;
    *found = result.found;
    *wall_ms = std::chrono::duration<double, std::milli>(finish - start).count();

    if (check_answer && result.found) {
        // 通过正式输出函数生成文本，再交给独立验证器。
        // 这样可以同时覆盖求解、方向映射、坐标转换和验证器。
        std::ostringstream answer;
        recover::PrintDirections(model, result.directions, &answer);
        std::istringstream answerInput(answer.str());
        const recover::ValidationResult validation =
            recover::ValidateOutput(model, &answerInput);
        if (validation.status != recover::ValidationStatus::kValid) {
            return "solver output failed validation: " + validation.message;
        }
    }
    return {};
}

void PrintHeader() {
    // 固定字段顺序，tools/run_benchmarks.py 依赖该顺序聚合结果。
    // 各字段含义：
    // case              输入文件名，不含绝对路径；
    // n,m               原矩阵尺寸；
    // connectors        非零连接器数量；
    // candidate_edges   候选直线栅栏数量；
    // conflict_pairs    空白格交叉冲突数量；
    // found             是否找到合法布局；
    // nodes             进入过的 DFS 节点数；
    // backtracks        回退次数；
    // propagation_events 事件队列处理连接器的次数；
    // forced_selections 强制选择边数；
    // forced_rejections 强制拒绝边数；
    // conflict_rejections 冲突传播拒绝边数；
    // max_depth         最大递归深度；
    // wall_ms           建模加求解的一次墙钟时间。
    std::cout
        << "case,n,m,connectors,candidate_edges,conflict_pairs,found,"
           "nodes,backtracks,propagation_events,forced_selections,"
           "forced_rejections,conflict_rejections,max_depth,wall_ms\n";
}

void PrintRow(const std::string& path, const recover::Model& model,
              const recover::SearchStats& stats, bool found, double wallMs) {
#if defined(RECOVER_LEGACY_BASELINE)
    // 95d34ba 的 SearchStats 还没有传播字段；
    // 兼容基准把缺失字段写成 0，而不是伪造它们存在。
    const std::uint64_t propagationEvents = 0;
    const std::uint64_t forcedSelections = 0;
    const std::uint64_t forcedRejections = 0;
    const std::uint64_t conflictRejections = 0;
#else
    const std::uint64_t propagationEvents = stats.propagationEvents;
    const std::uint64_t forcedSelections = stats.forcedSelections;
    const std::uint64_t forcedRejections = stats.forcedRejections;
    const std::uint64_t conflictRejections = stats.conflictRejections;
#endif

    // 文件 stem 不含目录和扩展名，因此 CSV 中不会暴露绝对路径。
    // 模型规模列帮助解释不同类别之间的耗时和节点差异。
    // 搜索统计列区分传播、冲突拒绝和最终 DFS 结构。
    // wall_ms 是单次重复的原始值，聚合脚本负责计算中位数和最大值。
    std::cout << std::filesystem::path(path).stem().string() << ','
              << model.rows << ',' << model.cols << ','
              << model.vertexCount << ',' << model.CandidateEdgeCount() << ','
              << model.conflictPairCount << ',' << (found ? 1 : 0) << ','
              << stats.nodes << ',' << stats.backtracks << ','
              << propagationEvents << ',' << forcedSelections
              << ',' << forcedRejections << ','
              << conflictRejections << ',' << stats.maximumDepth << ','
              << wallMs << '\n';
}

recover::Model BuildMetadata(const std::string& path) {
    // 计时结束后重新构建一次模型，仅用于读取候选边和冲突数量。
    // 该开销不计入 wall_ms，也不会修改原始输入文件。
    recover::Grid grid;
    std::string error;
    recover::Model model;
    if (!LoadCase(path, &grid, &error) ||
        !recover::BuildModel(grid, &model, &error)) {
        return model;
    }
    return model;
}

}  // namespace

int main(int argc, char** argv) {
    // 默认每个文件正式测量 5 次，另做 1 次不计时预热。
    // 预热可以减少首次分配和系统缓存影响，但不会产生 CSV 行。
    int repetitions = 5;
    int firstPath = 1;
    if (argc >= 3 && std::string(argv[1]) == "--repeat") {
        // 解析重复次数，并允许从第三个参数开始传入多个案例。
        std::istringstream count(argv[2]);
        if (!(count >> repetitions) || repetitions < 1) {
            std::cerr << "Invalid repetition count\n";
            return 2;
        }
        firstPath = 3;
    }
    if (firstPath >= argc) {
        // 没有输入案例时直接报用法错误，不生成空表头数据文件。
        std::cerr << "Usage: recover_benchmark [--repeat N] <case>...\n";
        return 2;
    }

    // 表头只打印一次；后续每次重复追加一行原始数据。
    PrintHeader();
    for (int argument = firstPath; argument < argc; ++argument) {
        // 每个案例先运行一次不测量的预热。
        const std::string path = argv[argument];

        recover::SearchStats warmStats;
        bool warmFound = false;
        double warmMs = 0.0;
        // 预热不验证答案，只为后续计时减少首次分配和缓存影响。
        const std::string warmError =
            RunOnce(path, false, &warmStats, &warmFound, &warmMs);
        if (!warmError.empty()) {
            // 预热失败立即终止，避免将坏输入误记成慢算法。
            std::cerr << path << ": " << warmError << '\n';
            return 1;
        }

        for (int repeat = 0; repeat < repetitions; ++repeat) {
            // 每次重复都重新建立 Solver，避免跨次搜索状态污染。
            recover::SearchStats stats;
            bool found = false;
            double wallMs = 0.0;
            // 正式运行启用独立答案验证，保证基准不会收录错误解。
            const std::string error =
                RunOnce(path, true, &stats, &found, &wallMs);
            if (!error.empty()) {
                // 正式测量失败时返回非零退出码，驱动脚本会记录 error。
                std::cerr << path << ": " << error << '\n';
                return 1;
            }
            // 模型元数据在计时之外构建，只用于报告 candidate_edges 等规模。
            const recover::Model model = BuildMetadata(path);
            PrintRow(path, model, stats, found, wallMs);
        }
    }
    return 0;
}
