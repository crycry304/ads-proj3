#include "model.hpp"

#include <queue>

namespace recover {
namespace {

// 本文件完成候选模型的静态构造，完全不进行搜索。
// BuildModel 的执行顺序不能随意交换：
// 1. 检查矩阵形状；
// 2. 给非零位置分配顶点编号；
// 3. 生成水平、竖直候选长边；
// 4. 做必要的奇偶和容量检查；
// 5. 扫描空白格，构造对称交叉冲突表。
//
// 候选边生成的直观例子：
// 行 "1 0 0 2 0 3" 有三个非零连接器。
// 候选水平边是 1--2 和 2--3，而不是 1与2、2与3之间每个零格都建边。
// 1--2 这条边表示从列 1 直通到列 4，中间两个零不会形成拐角。
//
// 冲突表的直观例子：
// 若某水平长边穿过空白格 (r,c)，某竖直长边也穿过同一个 (r,c)，
// 则两条边不能同时出现，否则会在没有连接器的地方交叉。
// 冲突关系必须双向保存，因为无论先选中哪一条，都要拒绝另一条。

// 添加一条无向候选栅栏。
// 这里同时做四件事：
// 1. 记录两端的反向方向；
// 2. 记录两端坐标；
// 3. 把边加入全局 edges；
// 4. 更新 incidentEdges 和 directionEdge 的索引。
//
// directionAtU 与 directionAtV 必须互相对应：
// 水平边通常是 (right, left)；
// 竖直边通常是 (down, up)。
// 如果端点顺序或方向写反，验证器会认为栅栏只有一端存在。
int AddEdge(Model* model, int u, int v, int directionAtU, int directionAtV) {
    // 顶点编号是在 BuildModel 中按行优先分配的，因此坐标可以直接查出。
    CandidateEdge edge;
    // 先把段的两个端点和方向写完整，后续任何地方都靠这些字段恢复输出。
    edge.u = u;
    edge.v = v;
    edge.directionAtU = directionAtU;
    edge.directionAtV = directionAtV;
    edge.row1 = model->vertexRow[u];
    edge.col1 = model->vertexCol[u];
    edge.row2 = model->vertexRow[v];
    edge.col2 = model->vertexCol[v];
    // 对水平边，row1 == row2；对竖直边，col1 == col2。
    // 坐标字段既用于判断方向，也用于之后扫描长边内部的空白位置。

    // 候选边编号按生成顺序分配，后续求解器和验证器都使用这个稳定编号。
    const int edgeId = static_cast<int>(model->edges.size());
    model->edges.push_back(edge);
    // 同时更新边到顶点的反向索引和顶点到方向的索引。
    model->incidentEdges[u].push_back(edgeId);
    model->incidentEdges[v].push_back(edgeId);
    model->directionEdge[u][directionAtU] = edgeId;
    model->directionEdge[v][directionAtV] = edgeId;
    return edgeId;
}

// 对候选图的每个连通分量做必要条件检查。
// 同一分量内的边不能帮助其他分量满足度数，所以在每个分量内：
// - 需求总度数必须是偶数；
// - 候选边数至少要有 demand / 2 条。
bool CheckCandidateComponents(Model* model) {
    // component[v] 保存 v 所在分量的起始顶点，-1 表示尚未访问。
    std::vector<int> component(model->vertexCount, -1);
    for (int start = 0; start < model->vertexCount; ++start) {
        // 已经访问过的顶点属于之前处理过的分量，不再重复 BFS。
        if (component[start] != -1) {
            continue;
        }

        std::queue<int> pending;
        // 分量的需求和边数必须单独计算，不能把不同分量的度数混在一起。
        pending.push(start);
        component[start] = start;
        int requiredDegree = 0;
        int endpointOccurrences = 0;

        while (!pending.empty()) {
            // BFS 收集当前候选分量的全部连接器。
            const int u = pending.front();
            pending.pop();
            requiredDegree += model->requiredDegree[u];

            // 每个候选边的两个端点都会累加一次，最后除以 2 得到边数。
            for (int edgeId : model->incidentEdges[u]) {
                // 每条边从两个端点各遍历一次，因此这个计数最后要除以 2。
                ++endpointOccurrences;
                const CandidateEdge& edge = model->edges[edgeId];
                const int v = edge.u == u ? edge.v : edge.u;
                if (component[v] == -1) {
                    // 首次访问邻接顶点时标记所属分量并入队。
                    component[v] = start;
                    pending.push(v);
                }
            }
        }

        // incidentEdges 会从每条边的两个端点各访问一次，所以除以 2。
        const int componentEdgeCount = endpointOccurrences / 2;
        if (requiredDegree % 2 != 0 ||
            componentEdgeCount < requiredDegree / 2) {
            // 这两个条件只说明无法满足局部需求，不依赖交叉表。
            model->structurallyPossible = false;
            model->impossibleReason =
                "a candidate component cannot satisfy its degree demand";
            return false;
        }
    }
    return true;
}

}  // namespace

bool BuildModel(const Grid& input, Model* model, std::string* error) {
    // BuildModel 只建立静态模型，不决定任何候选边是否被选中。
    // 同一实例重复构建应得到完全相同的顶点编号和冲突表。
    //
    // 构建结束后必须保持以下不变量：
    // - vertexAt 中非负值恰好覆盖 [0, vertexCount)；
    // - 每条候选边在两端 incidentEdges 中各出现一次；
    // - 每个方向最多对应一条候选边；
    // - conflictEdges 始终对称；
    // - 候选边编号等于 edges 中的下标。
    // 输出参数先检查，避免无效指针导致后续写崩。
    if (model == nullptr) {
        if (error != nullptr) {
            *error = "null model output";
        }
        return false;
    }

    Model built;
    // 先在最外层检查行列形状，后面各阶段都依赖矩形矩阵。
    built.grid = input;
    built.rows = static_cast<int>(input.size());
    built.cols = built.rows == 0 ? 0 : static_cast<int>(input[0].size());
    if (built.rows <= 1 || built.cols <= 1) {
        if (error != nullptr) {
            *error = "grid dimensions are invalid";
        }
        return false;
    }
    for (const auto& row : input) {
        // 只有所有行长度一致，后面用 r*cols+c 做一维编号才安全。
        if (static_cast<int>(row.size()) != built.cols) {
            if (error != nullptr) {
                *error = "grid is not rectangular";
            }
            return false;
        }
    }

    // 为所有非零连接器分配紧密编号。
    // 零位置保持 -1，表示后续既不能输出，也不能作为栅栏端点。
    built.vertexAt.assign(built.rows, std::vector<int>(built.cols, -1));
    for (int r = 0; r < built.rows; ++r) {
        for (int c = 0; c < built.cols; ++c) {
            const int degree = input[r][c];
            if (degree < 0 || degree > 4) {
                if (error != nullptr) {
                    *error = "connector degree is outside [0, 4]";
                }
                return false;
            }
            if (degree == 0) {
                // 零位置没有连接器，不分配顶点编号。
                continue;
            }

            // 顶点编号严格按行优先顺序产生，验证器因此可以直接检查输出顺序。
            const int id = built.vertexCount++;
            // id 从 0 连续增长，用于 incidentEdges 和 directionEdge 下标。
            // 行优先分配保证这个编号顺序天然等于题目要求的输出顺序。
            built.vertexAt[r][c] = id;
            built.vertexRow.push_back(r);
            built.vertexCol.push_back(c);
            built.requiredDegree.push_back(degree);
            built.totalRequiredDegree += degree;
        }
    }

    // 初始化每个顶点与四个方向的索引。
    // directionEdge 初始化为 -1，候选边生成后再填入真实编号。
    built.incidentEdges.resize(built.vertexCount);
    built.directionEdge.assign(
        built.vertexCount, std::array<int, 4>{-1, -1, -1, -1});

    // 每行的相邻非零连接器之间形成一条水平候选长边。
    // 这里遍历所有零位置并跳过，所以一条候选边可以跨越任意多个空白列。
    for (int r = 0; r < built.rows; ++r) {
        // previous 记录本行上一个遇到的非零顶点。
        // 这样即使中间有很多零，也只生成一条连续候选长边。
        int previous = -1;
        for (int c = 0; c < built.cols; ++c) {
            const int current = built.vertexAt[r][c];
            if (current == -1) {
                // 零位置只影响栅栏的视觉长度，不会成为候选边端点。
                continue;
            }
            if (previous != -1) {
                // 左边顶点向右、右边顶点向左，形成同一条无向候选边。
                AddEdge(&built, previous, current, kRight, kLeft);
            }
            // 当前非零连接器成为下一次生成水平候选边的左端点。
            previous = current;
        }
    }

    // 每列的相邻非零连接器之间形成一条竖直候选长边。
    // 同样允许跨过中间的零位置，但遇到下一个非零连接器就必须分段。
    for (int c = 0; c < built.cols; ++c) {
        // 竖直线索与水平处理完全对称，只是扫描方向换成从上到下。
        int previous = -1;
        for (int r = 0; r < built.rows; ++r) {
            const int current = built.vertexAt[r][c];
            if (current == -1) {
                continue;
            }
            if (previous != -1) {
                // 上方顶点向下、下方顶点向上。
                AddEdge(&built, previous, current, kDown, kUp);
            }
            // 当前非零连接器成为下一次生成竖直候选边的上端点。
            previous = current;
        }
    }

    // 每条被选栅栏贡献 2 度，因此总需求为奇数时一定无解。
    // 候选边数量不足 demand/2 也是立即可以排除的情况。
    if (built.totalRequiredDegree % 2 != 0 ||
        built.CandidateEdgeCount() < built.totalRequiredDegree / 2) {
        // 记录导致不可行的原因，方便 model-info 和测试输出定位。
        built.structurallyPossible = false;
        built.impossibleReason =
            "odd total degree or too few candidate edges";
    }

    for (int v = 0; v < built.vertexCount; ++v) {
        // 连接器最多只有四条候选边，方向数少于要求度数时无解。
        if (static_cast<int>(built.incidentEdges[v].size()) <
            built.requiredDegree[v]) {
            built.structurallyPossible = false;
            // 发现第一条失败原因即可停止进一步静态检查。
            built.impossibleReason =
                "a connector has too few possible incident segments";
            break;
        }
    }

    if (built.structurallyPossible) {
        // 只有全局必要条件通过后，才继续检查各候选分量。
        CheckCandidateComponents(&built);
    }

    // 同一行/列的候选区间互不重叠，所以每个空白格至多有一对交叉候选边。
    // horizontalAt/verticalAt 先记录穿过每个格子的唯一边编号。
    std::vector<int> horizontalAt(built.rows * built.cols, -1);
    // 两个数组都以展平后的一维格点下标作为索引：
    // index = row * cols + col。
    std::vector<int> verticalAt(built.rows * built.cols, -1);
    for (int edgeId = 0; edgeId < built.CandidateEdgeCount(); ++edgeId) {
        const CandidateEdge& edge = built.edges[edgeId];
        if (edge.IsHorizontal()) {
            // 只标记严格内部位置，端点本身是连接器，不属于交叉冲突。
            for (int c = edge.col1 + 1; c < edge.col2; ++c) {
                horizontalAt[edge.row1 * built.cols + c] = edgeId;
            }
        } else {
            // 竖直段同理，只扫描两端之间的内部行。
            for (int r = edge.row1 + 1; r < edge.row2; ++r) {
                verticalAt[r * built.cols + edge.col1] = edgeId;
            }
        }
    }

    // 只有零位置才禁止水平/竖直同时穿过；连接器位置允许相交或拐弯。
    built.conflictEdges.resize(built.CandidateEdgeCount());
    for (int r = 0; r < built.rows; ++r) {
        for (int c = 0; c < built.cols; ++c) {
            // 非零格点是连接器，允许水平、竖直在那里相交，不建立冲突。
            if (built.grid[r][c] != 0) {
                continue;
            }
            const int horizontal = horizontalAt[r * built.cols + c];
            const int vertical = verticalAt[r * built.cols + c];
            if (horizontal != -1 && vertical != -1) {
                // 冲突是无向关系，因此两个边编号都要加入对方的邻接表。
                built.conflictEdges[horizontal].push_back(vertical);
                built.conflictEdges[vertical].push_back(horizontal);
                ++built.conflictPairCount;
            }
        }
    }

    // 用 move 避免复制整个矩阵和全部候选边。
    *model = std::move(built);
    return true;
}

}  // namespace recover
