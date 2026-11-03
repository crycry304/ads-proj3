#include "io.hpp"
#include "model.hpp"
#include "solver.hpp"
#include "validator.hpp"

#include <fstream>
#include <iostream>
#include <string>

namespace {

// 当前命令行同时充当正式程序入口和调试/验证入口。
// 帮助信息输出到 stderr，保证正式答案的 stdout 不混入提示文字。
// 调试命令设计：
// --model-info 只打印候选模型规模，不尝试求解；
// --validate 读取一个候选答案并单独验证它。
// 后续求解器使用无参数模式时，标准输出只能包含题目答案。
//
// 没有参数时目前只打印 usage，不会伪造 No Solution。
// 这样在求解器尚未接入时，不会让测试误以为某个输入真的无解。
// 所有调试输出要么写到文件内容，要么使用 stderr。
void PrintUsage() {
    std::cerr
        << "Usage:\n"
        << "  recover < input\n"
        << "  recover --validate <input> <output>\n"
        << "  recover --model-info <input>\n";
}

}  // namespace

int main(int argc, char** argv) {
    // 输入最多只有 2500 个整数，普通 cin/cout 已足够快。
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    // 无参数时从 stdin 读取并求解；调试模式从文件读取。
    // 退出码约定：
    // 0 = 命令成功；
    // 1 = 候选答案无效；
    // 2 = 参数、文件或输入格式错误；
    // 3 = 候选答案文本为 No Solution。
    if (argc != 1 && argc != 3 && argc != 4) {
        PrintUsage();
        return 2;
    }

    if (argc == 1) {
        // 正式评测路径：从 stdin 读取唯一测试用例。
        recover::Grid grid;
        std::string error;
        if (!recover::ReadSingleCase(&std::cin, &grid, &error)) {
            std::cerr << error << '\n';
            return 2;
        }

        recover::Model model;
        if (!recover::BuildModel(grid, &model, &error)) {
            std::cerr << error << '\n';
            return 2;
        }

        recover::Solver solver(model);
        const recover::SolveResult result = solver.Solve();
        // 求解器返回 false 时，当前基线已经完整穷尽搜索树。
        if (result.found) {
            recover::PrintDirections(model, result.directions, &std::cout);
        } else {
            recover::PrintNoSolution(&std::cout);
        }
        return 0;
    }

    const std::string mode = argv[1];
    // 调试模式从文件读取；最终标准求解模式仍会从 std::cin 读取。
    std::ifstream inputFile(argv[2]);
    if (!inputFile) {
        std::cerr << "Cannot open input file\n";
        return 2;
    }

    recover::Grid grid;
    std::string error;
    // 解析阶段只检查输入形状和数值范围，不判断题目是否有解。
    if (!recover::ReadSingleCase(&inputFile, &grid, &error)) {
        std::cerr << error << '\n';
        return 2;
    }

    recover::Model model;
    // 建模阶段建立候选边与冲突表，但暂时不执行搜索。
    if (!recover::BuildModel(grid, &model, &error)) {
        std::cerr << error << '\n';
        return 2;
    }

    if (mode == "--model-info" && argc == 3) {
        // 打印模型统计量，方便检查候选边和冲突表有没有构造正确。
        std::cout << "rows " << model.rows << '\n'
                  << "cols " << model.cols << '\n'
                  << "connectors " << model.vertexCount << '\n'
                  << "candidate_edges " << model.CandidateEdgeCount() << '\n'
                  << "conflict_pairs " << model.conflictPairCount << '\n'
                  << "structurally_possible "
                  << (model.structurallyPossible ? 1 : 0) << '\n';
        return 0;
    }

    if (mode == "--validate" && argc == 4) {
        // 验证模式必须同时提供输入文件和候选输出文件。
        std::ifstream outputFile(argv[3]);
        if (!outputFile) {
            std::cerr << "Cannot open output file\n";
            return 2;
        }
        // 验证器返回三种状态，其中 No Solution 不等于已经证明无解。
        const recover::ValidationResult validation =
            recover::ValidateOutput(model, &outputFile);
        // 调试模式只打印验证摘要；正式求解模式只打印题目答案。
        std::cout << validation.message << '\n';
        // 0 表示布局合法，1 表示格式或内容错误，3 表示输出声称无解。
        if (validation.status == recover::ValidationStatus::kValid) {
            return 0;
        }
        if (validation.status == recover::ValidationStatus::kNoSolution) {
            return 3;
        }
        return 1;
    }

    PrintUsage();
    return 2;
}
