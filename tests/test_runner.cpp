#include "io.hpp"
#include "model.hpp"
#include "validator.hpp"

#include <iostream>
#include <sstream>
#include <string>
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

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All model and validation tests passed\n";
    return 0;
}
