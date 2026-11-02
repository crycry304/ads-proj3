#include "io.hpp"
#include "model.hpp"

#include <sstream>

namespace recover {

// 解析流程：
// 1. 读取 n、m 并检查数据范围；
// 2. 读取 n*m 个整数；
// 3. 检查每个度数是否在 0..4；
// 4. 确认测试用例后没有额外 token；
// 5. 只有全部成功后才修改调用者传入的 grid。
//
// 这里使用局部变量 parsed，避免解析失败时留下半份网格。
//
// 输出函数同样属于 IO：
// 它只把已经确定的方向数组按题面格式写出，
// 不检查方向是否满足度数，也不判断交叉。
// 输出和验证分开后，任何格式错误都能由 validator 独立复现。
bool ReadSingleCase(std::istream* input, Grid* grid, std::string* error) {
    // error 是可选诊断输出；调用者如果不关心原因，可以传 nullptr。
    // 题面保证每个输入文件只有一个测试用例，因此这里不读取额外的测试数。
    int rows = 0;
    int cols = 0;
    // operator>> 会自动跳过分隔空白，因此题目中的空格和换行都不影响读取。
    if (!(*input >> rows >> cols)) {
        // 连第一行都读不到时，没有必要继续读取矩阵。
        if (error != nullptr) {
            *error = "missing n or m";
        }
        return false;
    }

    if (rows <= 1 || rows > 50 || cols <= 1 || cols > 50) {
        // 范围检查放在分配矩阵之前，避免由非法维度造成过大分配。
        if (error != nullptr) {
            *error = "n and m must satisfy 2 <= n,m <= 50";
        }
        return false;
    }

    Grid parsed(rows, std::vector<int>(cols, 0));
    // 逐格读取矩阵，缺数、非整数或度数越界都会在这里被发现。
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            // operator>> 读入失败可能表示提前 EOF，也可能表示非数字文本。
            if (!(*input >> parsed[r][c])) {
                if (error != nullptr) {
                    *error = "grid is shorter than n*m";
                }
                return false;
            }
            // 一个连接器最多连接上下左右四条边，因此合法度数是 0..4。
            if (parsed[r][c] < 0 || parsed[r][c] > 4) {
                if (error != nullptr) {
                    *error = "every connector degree must be in [0, 4]";
                }
                return false;
            }
            // 到这里 parsed[r][c] 已经是一个合法的 0..4 整数。
        }
    }

    // 题目只允许一个测试用例；读取到额外 token 说明输入格式不符合约定。
    // 这里不忽略剩余内容，避免把多组测试数据误当成单组数据的一部分。
    std::string extra;
    if (*input >> extra) {
        if (error != nullptr) {
            *error = "extra token after the single test case";
        }
        return false;
    }

    // 到这一步输入已经完整通过检查，才把结果移交给调用者。
    // move 避免复制最多 2500 个整数，也明确表示解析结果已经转交。
    *grid = std::move(parsed);
    return true;
}

// 输出阶段的唯一数据来源是 Model 与 directions。
// vertexAt 负责把内部编号重新映射回二维位置。
void PrintDirections(const Model& model,
                     const std::vector<std::array<int, 4>>& directions,
                     std::ostream* output) {
    // 输出顺序是题目格式的一部分：行优先，并且跳过所有零位置。
    for (int r = 0; r < model.rows; ++r) {
        for (int c = 0; c < model.cols; ++c) {
            const int vertex = model.vertexAt[r][c];
            // -1 表示该格没有连接器，题目要求完全跳过这一行。
            if (vertex == -1) {
                continue;
            }

            // 题面坐标从 1 开始，而内部保存的 r、c 从 0 开始。
            *output << (r + 1) << ' ' << (c + 1);

            // 方向顺序固定为 up、down、left、right，与 Direction 枚举一致。
            // 这里逐项打印 0/1，不根据度数重新猜测方向。
            for (int direction = 0; direction < 4; ++direction) {
                // 每行的格式为 row col up down left right，
                // 因此这里只追加方向和前导空格，不打印描述文字。
                *output << ' ' << directions[vertex][direction];
            }
            // 每行结束时输出换行，但不再额外输出行尾空格。
            *output << '\n';
        }
    }
}

// 无解输出必须与题面完全一致，因此集中在一个函数中维护。
void PrintNoSolution(std::ostream* output) {
    *output << "No Solution\n";
}

}  // namespace recover
