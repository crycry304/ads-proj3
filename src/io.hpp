#pragma once

#include <array>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace recover {

// IO 模块只处理“题目文本”和“内部数据”之间的转换。
// 它不判断答案是否存在，也不参与候选边的组合搜索。
//
// 输入侧：把标准输入转换为 Grid。
// 输出侧：把连接器方向数组转换为题目规定的逐行文本。
// 这样求解器、验证器和测试代码都不必重复处理坐标偏移与输出顺序。
//
// 例子：
// 内部坐标 (0,0) 在输出中应变成 1-based 的 "1 1"。
// 内部方向数组按 up/down/left/right 排列。
// 某方向为 1 时只表示该连接器在该方向有栅栏，并不单独证明对端存在；
// 对端一致性由模型候选边和验证器负责检查。

// Grid 直接对应题目输入的 n 行 m 列整数矩阵。
using Grid = std::vector<std::vector<int>>;
struct Model;

// 读取唯一的测试用例。
// 成功时写入 grid，失败时写入 error，并返回 false。
// 读取失败时不会修改调用者原来的 grid，避免出现部分初始化的数据。
bool ReadSingleCase(std::istream* input, Grid* grid, std::string* error);

// 按题目要求输出所有非零连接器，顺序固定为从上到下、从左到右。
// directions[v][d] 中的 v 是 Model 内部顶点编号，d 的顺序为上下左右。
// 函数不排序答案，因为 Model 的 vertexAt 已经保证行优先编号。
void PrintDirections(const Model& model,
                     const std::vector<std::array<int, 4>>& directions,
                     std::ostream* output);

// 无合法设计时统一由 IO 模块输出题面要求的固定字符串。
// 这个函数同时避免多处手写大小写不一致的 "No Solution"。
void PrintNoSolution(std::ostream* output);

}  // namespace recover
