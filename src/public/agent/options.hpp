/// @file options.hpp
/// @brief 核心的上下文与运行选项；外围装配输入见 setup.hpp。
#pragma once

#include <chrono>
#include <cstddef>
#include "agent/permission.hpp"

namespace dagent::agent {

/// @brief 上下文预算，对应 config "context" 段（docs/design/agent.md §8）。
struct ContextOptions {
    std::size_t window_tokens = 262144;
    std::size_t safety_margin_tokens = 8192;
    int compaction_trigger_percent = 80;
    int compaction_target_percent = 60;
};

/// @brief 一轮运行的调用上限，对应 "run" 段。
struct Limits {
    int max_model_calls = 24;
    int max_tool_calls = 35;
    int max_model_retries = 2;
    int max_parallel_tasks = 4; ///< 并发子 Agent 上限，下限 3；每个都要打模型请求，故低于只读组的 8
};

/// @brief 进度提示间隔，对应 "progress" 段。
struct ProgressOptions {
    std::chrono::milliseconds interval{1000}; ///< run 模式心跳、界面计时刷新
};

struct Options {
    ContextOptions context;
    Limits run;
    ProgressOptions progress;
    PermissionMode permissions = PermissionMode::workspace;
    bool read_only = false;
};

} // namespace dagent::agent
