#pragma once

#include "io.hpp"

#include <array>
#include <string>
#include <vector>

namespace recover {

// 本文件定义“候选模型”的数据结构。
// 候选模型只描述题目中有哪些可选择的直线栅栏，以及哪些候选栅栏互相冲突。
// 它不保存某一次 DFS 的选择状态；那些动态状态将在 Solver 中维护。
//
// 数据组织的主线：
// 输入矩阵 -> 非零连接器顶点 -> 相邻顶点候选长边 -> 空白格交叉冲突。
//
// 命名约定：
// - vertexCount 是顶点数量，不是二维矩阵面积；
// - edgeCount 是候选边数量，不是最终选择方案的数量；
// - directionEdge 从顶点查边；
// - incidentEdges 从顶点查边列表；
// - conflictEdges 从边查边。
// 数组下标统一使用 0-based，题目输出坐标再转换为 1-based。

// 连接器的四个方向顺序必须与题目输出顺序完全一致。
// 虽然枚举从 0 开始，但输出时只打印方向对应的 0/1 状态。
enum Direction : int {
    kUp = 0,
    kDown = 1,
    kLeft = 2,
    kRight = 3,
};

// 一条候选长边表示同一行/列中相邻两个非零连接器之间的整段栅栏。
// 栅栏中间允许有零位置，因此不能只连接二维矩阵中相邻的格子。
// u、v 是两端连接器的内部编号；directionAtU、directionAtV 是反向方向。
struct CandidateEdge {
    // 候选边的两个端点，均使用密集顶点编号而不是二维坐标。
    // u 是生成候选边时先遇到的连接器。
    int u = -1;

    // v 是在同一行/列中后遇到的连接器。
    int v = -1;

    // 如果从 u 看向 v，方向是 directionAtU；
    // 从 v 看向 u 的区别就是左右互换或上下互换。
    int directionAtU = 0;

    // 水平边通常为 kRight/kLeft，竖直边通常为 kDown/kUp。
    int directionAtV = 0;

    // 保存原始坐标，构建冲突表时可以直接扫描边内部经过的格子。
    int row1 = 0;
    int col1 = 0;
    int row2 = 0;
    int col2 = 0;

    // 两端行号相同即可判断为水平段，不需要再额外保存布尔类型。
    bool IsHorizontal() const {
        return row1 == row2;
    }
};

struct Model {
    // 矩阵规模和连接器、度数的汇总统计。
    // rows、cols 是原始题目尺寸。
    int rows = 0;
    int cols = 0;

    // vertexCount 是所有非零格子的数量，也就是图的顶点数。
    int vertexCount = 0;

    // totalRequiredDegree 是所有连接器度数的总和。
    // 每条栅栏贡献 2 度，所以它必须是偶数，目标边数是它的一半。
    int totalRequiredDegree = 0;

    // conflictPairCount 是空白格交叉对的数量，用于模型测试和性能统计。
    int conflictPairCount = 0;

    // grid 保留原输入，vertexAt 把二维坐标映射为 [0, vertexCount) 的编号。
    // 新增一个非零位置时，vertexAt 中对应值由 -1 改成它的密集编号。
    Grid grid;

    // vertexAt[r][c] == -1 表示零位置。
    // 其他值 v 可以直接作为 requiredDegree、incidentEdges 的下标。
    std::vector<std::vector<int>> vertexAt;

    // 顶点坐标用于构造候选边，也用于输出和验证时的坐标转换。
    // 下标就是顶点编号，因此 vertexRow[v] 与 vertexCol[v] 必须同步增长。
    std::vector<int> vertexRow;
    std::vector<int> vertexCol;

    // requiredDegree[v] 是输入规定的连接器度数。
    // 求解时最终必须精确等于这个值，不能多选也不能少选。
    std::vector<int> requiredDegree;

    // directionEdge[v][d] 是该方向上的候选边编号，-1 表示不存在。
    // 因为每个方向的“下一个非零连接器”唯一，所以一个位置至多一条候选边。
    // 例如 (r,c) 向右的候选边会在右侧第一个非零连接器的 kLeft 处再次出现。
    std::vector<std::array<int, 4>> directionEdge;

    // incidentEdges[v] 保存所有与 v 相连的候选边编号，最多四条。
    // 求解器只遍历这个紧凑列表，不需要每次扫描整个二维矩阵。
    // 同一连接器出现两次相同候选边说明建模出错。
    std::vector<std::vector<int>> incidentEdges;

    // edges 是整个实例的候选长边集合，是否被选择由求解器维护。
    // 候选边编号一经建立便保持不变，冲突表和测试都依赖这个稳定编号。
    // 求解器通常用 vector<char> 或 vector<uint8_t> 按此编号保存三态。
    std::vector<CandidateEdge> edges;

    // conflictEdges 是对称邻接表；冲突只发生在空白格内部交叉处。
    // 同一空白格至多产生一对水平/竖直候选边冲突。
    // 因而整张冲突图的边数至多与 n*m 同阶。
    std::vector<std::vector<int>> conflictEdges;

    // 静态检查失败时一定无解；检查成功只说明必要条件是满足的。
    // 因此 structurallyPossible=true 不能直接输出答案，也不能跳过搜索。
    bool structurallyPossible = true;

    // 保存第一条静态失败原因，便于测试和调试定位。
    // 求解器只有在完整搜索结束后才可以把错误文本设为 No Solution。
    std::string impossibleReason;

    // 候选边总数就是 edges.size()，此函数避免调用处反复做类型转换。
    int CandidateEdgeCount() const {
        return static_cast<int>(edges.size());
    }
};

// 根据输入矩阵建立全部连接器、候选边和交叉冲突。
// 只有输入形状本身错误时才返回 false；题目无解也属于建模成功。
bool BuildModel(const Grid& input, Model* model, std::string* error);

}  // namespace recover
