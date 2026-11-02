#pragma once

#include "model.hpp"

#include <array>
#include <istream>
#include <string>
#include <vector>

namespace recover {

// 验证器与求解器完全解耦。
// 求解器可能依赖内部边状态和搜索痕迹，但验证器只接受：
// 1. 由输入构建出的 Model；
// 2. 一份文本答案。
// 它重新计算连接器度数、栅栏端点和交叉关系，可以独立发现求解器错误。
//
// ValidationResult 的设计：
// - status 给调用者做程序分支；
// - message 给人和测试日志阅读；
// - directions 只在合法时使用，保存解析后的结构化答案。
// 调用者不应根据 message 字符串判断状态，而应使用 status。

// kValid 表示构造出了合法布局；kNoSolution 只表示输出文本声称无解。
enum class ValidationStatus {
    // 答案已经逐项验证通过。
    kValid,

    // 答案文本是 No Solution，但仅凭该文本无法证明实例真的无解。
    kNoSolution,

    // 格式、度数、端点配对或交叉关系至少有一项不合法。
    kInvalid,
};

// 验证成功后同时返回方向数组，便于测试输出模块的往返一致性。
struct ValidationResult {
    // status 决定调用方的处理方式；message 提供可读错误原因。
    ValidationStatus status = ValidationStatus::kInvalid;
    std::string message;

    // 只在 kValid 时填充，供其他测试再次打印或比较。
    std::vector<std::array<int, 4>> directions;
};

// 验证器只读取输入模型和候选输出，不依赖求解器的内部搜索状态。
ValidationResult ValidateOutput(const Model& model, std::istream* output);

}  // namespace recover
