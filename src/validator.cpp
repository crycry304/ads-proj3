#include "validator.hpp"

#include <sstream>

namespace recover {
namespace {

// 以下两个辅助函数只处理输出文本本身的格式，不依赖求解器内部状态。
//
// 验证器故意不读取求解器的 selected/rejected 状态。
// 即使求解器内部认为某个方向正确，也必须从最终文本重新构造信息：
// 根据两端方向找出被选中的候选栅栏，再重算度数并检查冲突。
// 这样可以发现“内部状态正确但输出时坐标写错”的问题。
//
// 验证顺序是有意的：
// 先做最便宜的文本格式检查，再做坐标和覆盖检查，
// 最后才重建候选边并检查度数与冲突。
// 这样错误答案会在最早阶段失败，报告原因也更明确。
//
// ValidateOutput 不修改 Model，也不修改输出文本以外的程序状态。
// 同一模型可以用不同候选文本重复调用，适合随机对拍和调试。

// Windows 文本文件可能带有 '\r'；在格式检查前先移除行尾回车。
std::string RemoveCarriageReturn(std::string line) {
    // 只删除一个行尾 '\r'，不顺手 trim 空格；
    // 前导和尾随空格需要单独报错，不能在这里悄悄修正。
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

// 用于确认 "No Solution" 之后除了空白字符以外没有其他内容。
bool IsOnlyWhitespace(std::istream* input) {
    char ch = '\0';
    // noskipws 很关键，否则 operator>> 会自动跳过空格，无法检查杂项字符。
    while (*input >> std::noskipws >> ch) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            return false;
        }
    }
    return true;
}

}  // namespace

// 独立验证一份候选输出，检查顺序：
// 1. 特殊空输出与 "No Solution"；
// 2. 每行坐标和方向格式；
// 3. 连接器完整性与行优先顺序；
// 4. 精确度数、栅栏两端一致性；
// 5. 空白格交叉冲突。
ValidationResult ValidateOutput(const Model& model, std::istream* output) {
    // 每个失败分支都返回具体原因，方便答案验证器和测试报告定位问题。
    ValidationResult result;
    std::string line;
    if (!std::getline(*output, line)) {
        // 全零输入没有连接器，此时空输出本身就是合法布局。
        if (model.vertexCount == 0) {
            result.status = ValidationStatus::kValid;
            result.message = "valid empty layout";
            return result;
        }
        result.message = "empty output";
        return result;
    }
    line = RemoveCarriageReturn(line);

    // 验证器只能识别“声称无解”，无法仅凭输出文件证明不存在解。
    if (line == "No Solution") {
        if (!IsOnlyWhitespace(output)) {
            result.message = "extra content after No Solution";
            return result;
        }
        result.status = ValidationStatus::kNoSolution;
        result.message = "output claims no solution";
        return result;
    }

    // 解析成功后保存每个连接器的四方向，最终也作为返回值交给输出模块。
    std::vector<std::array<int, 4>> directions(
        model.vertexCount, std::array<int, 4>{0, 0, 0, 0});
    // seen 用来同时发现遗漏和重复；每个连接器最终只能被输出一次。
    std::vector<bool> seen(model.vertexCount, false);

    // expectedVertex 利用顶点按行优先编号的性质检查输出顺序。
    int expectedVertex = 0;
    // first 用来识别“文件非空但没有任何连接器行”的情况。
    bool first = true;
    while (true) {
        // 去掉 Windows 换行留下的 '\r'，其余空白仍需严格检查。
        if (line != RemoveCarriageReturn(line)) {
            line = RemoveCarriageReturn(line);
        }
        if (line.empty()) {
            // 合法答案不允许空行，避免不同判题器解释不一致。
            result.message = "blank output line";
            return result;
        }
        // 题目不允许多余的前导或行末空格。
        // std::istringstream 本身会跳过空格，所以必须在解析前看原始行首尾。
        if (line.front() == ' ' || line.back() == ' ' ||
            line.front() == '\t' || line.back() == '\t') {
            result.message = "leading or trailing whitespace";
            return result;
        }

        // 每行固定为 row、col、up、down、left、right 六个数。
        std::istringstream rowStream(line);
        int row = 0;
        int col = 0;
        std::array<int, 4> flags{};
        if (!(rowStream >> row >> col >> flags[0] >> flags[1] >> flags[2] >>
              flags[3])) {
            // row、col 和四个方向必须全部存在。
            result.message = "malformed connector line";
            return result;
        }
        // 注意 flags 的序号即 Direction 枚举：
        // flags[0]=up, flags[1]=down, flags[2]=left, flags[3]=right。
        std::string extra;
        if (rowStream >> extra) {
            // 每行恰好六个数，第七个 token 也视为错误。
            result.message = "extra token on connector line";
            return result;
        }

        // 输入坐标从 1 开始，内部顶点坐标从 0 开始。
        --row;
        --col;
        if (row < 0 || row >= model.rows || col < 0 || col >= model.cols) {
            result.message = "connector coordinate out of range";
            return result;
        }

        const int vertex = model.vertexAt[row][col];
        // -1 表示该坐标在原输入中是 0，不可能有连接器。
        if (vertex == -1) {
            result.message = "output contains a zero position";
            return result;
        }
        if (seen[vertex]) {
            // 同一个连接器不能出现两次。
            result.message = "duplicate connector";
            return result;
        }

        while (expectedVertex < model.vertexCount &&
               seen[expectedVertex]) {
            // 跳过已输出的前缀，下一次必须出现新的最小顶点编号。
            ++expectedVertex;
        }
        // 顶点编号按行优先产生，因此合法输出也必须按相同顺序出现。
        if (expectedVertex >= model.vertexCount || vertex != expectedVertex) {
            result.message = "connectors are not in row-major order";
            return result;
        }

        // 每个方向必须是 0/1，禁止出现 2 或其他非法状态。
        for (int direction = 0; direction < 4; ++direction) {
            if (flags[direction] != 0 && flags[direction] != 1) {
                result.message = "direction flags must be 0 or 1";
                return result;
            }
        }
        // 保存该连接器的方向状态，供后续整体约束检查。
        directions[vertex] = flags;
        seen[vertex] = true;
        first = false;

        if (!std::getline(*output, line)) {
            // EOF 表示所有连接器行读取结束，并不立即代表答案错误；
            // 后面的完整性和度数检查仍会继续。
            break;
        }
    }

    if (first) {
        // 只要模型有连接器，至少必须输出一行。
        result.message = "no connector lines";
        return result;
    }

    // 先检查每个连接器的度数是否与输入完全一致。
    for (int v = 0; v < model.vertexCount; ++v) {
        if (!seen[v]) {
            result.message = "missing connector";
            return result;
        }
        // 四个方向的 1 的数量就是该连接器实际连接的栅栏数。
        int degree = 0;
        for (int direction = 0; direction < 4; ++direction) {
            degree += directions[v][direction];
        }
        if (degree != model.requiredDegree[v]) {
            result.message = "wrong connector degree";
            return result;
        }
    }

    // 候选边只有两端方向同时为 1 时才表示整条栅栏被选中。
    // 例如 u 的 right=1、v 的 left=1，就说明 u--v 这条长边存在。
    std::vector<bool> selected(model.CandidateEdgeCount(), false);
    for (int edgeId = 0; edgeId < model.CandidateEdgeCount(); ++edgeId) {
        const CandidateEdge& edge = model.edges[edgeId];
        const int fromU = directions[edge.u][edge.directionAtU];
        const int fromV = directions[edge.v][edge.directionAtV];
        if (fromU != fromV) {
            // 候选长边必须同时被两端承认，否则栅栏会悬空。
            result.message = "fence exists at only one endpoint";
            return result;
        }
        selected[edgeId] = fromU == 1;
        // fromU == fromV == 0 时同样合法，只是说明这条候选边没有选择。
    }

    // 交叉冲突表已经预处理，验证器只需检查是否有冲突边被同时选中。
    for (int edgeId = 0; edgeId < model.CandidateEdgeCount(); ++edgeId) {
        if (!selected[edgeId]) {
            continue;
        }
        for (int other : model.conflictEdges[edgeId]) {
            if (selected[other]) {
                result.message = "selected fences cross at an empty cell";
                return result;
            }
        }
    }

    // 到这里为止所有结构约束都已通过。
    result.status = ValidationStatus::kValid;
    result.message = "valid connector layout";
    result.directions = std::move(directions);
    return result;
}

}  // namespace recover
