/// @file headless.hpp
/// @brief run 模式：非交互跑完一轮，按格式输出结果后退出。
#pragma once

#include <optional>
#include <string>

#include "agent/options.hpp"

namespace dagent::agent {

struct HeadlessOptions {
    enum class Output { text, json, jsonl };
    Output output = Output::text;
    std::string prompt; ///< app 已经合并了 stdin
    std::optional<std::string> resume_id;
    bool continue_last = false;
};

/// @brief 装配好的 Setup + 提示词 → 跑完一轮。返回进程退出码。
int run_headless(Setup setup, const HeadlessOptions&);

} // namespace dagent::agent
