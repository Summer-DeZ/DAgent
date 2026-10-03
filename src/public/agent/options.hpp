/// @file options.hpp
/// @brief 核心的上下文与运行选项；权限档与只读初值在 SessionConfig。
#pragma once

#include <chrono>
#include <cstddef>

namespace dagent::agent {

/// @brief 上下文预算，对应 config "context" 段（docs/design/agent.md §8）。
struct ContextOptions {
    std::size_t window_tokens = 0;
    std::size_t safety_margin_tokens = 0;
    int compaction_trigger_percent = 0;
    int compaction_target_percent = 0;
};

/// @brief 一轮运行的调用上限，对应 "run" 段。
struct Limits {
    int max_model_calls = 0;
    int max_tool_calls = 0;
    std::size_t max_total_tokens = 0; ///< 本 turn 模型输入+输出总预算，0 不限
    int max_model_retries = 0;
    int max_parallel_tasks = 0; ///< 并发子 Agent 上限
    int max_parallel_tools = 0; ///< 并发只读工具上限
};

/// @brief 进度提示间隔，对应 "progress" 段。
struct ProgressOptions {
    std::chrono::milliseconds interval{0}; ///< run 模式心跳、界面计时刷新
};

struct ParentApprovalOptions {
    bool parent_when_unrestricted = true;
    int max_reviews_per_turn = 16;
    std::chrono::milliseconds review_timeout{60000};
};

struct Options {
    ContextOptions context;
    Limits run;
    ProgressOptions progress;
    ParentApprovalOptions approval;
};

} // namespace dagent::agent
