/// @file dispatch.hpp
/// @brief ActionDispatcher：一个模型批次内动作的准备、分组、执行与按序提交。
///
/// 保留先准备、分组、必要时刷新准备、按槽位提交的算法；并行判定保持 B15。
/// 所有会话修改经 SessionCommitter，调度器不直接写对话、计划或记录。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agent/control.hpp"
#include "agent/message.hpp"
#include "agent/run_services.hpp"
#include "agent/session.hpp"

namespace dagent::agent {

class ActionDispatcher {
public:
    struct Outcome {
        enum class Stop { none, interrupted, denied } stop = Stop::none;
        int handled = 0;        ///< 这一批里计入 max_tool_calls 的调用数
        bool hit_limit = false; ///< 有调用因为超额拿到了 T8
    };

    ActionDispatcher(Session& session, Run& run, const RunServices& services)
        : session_(session), run_(run), services_(services) {}

    Outcome dispatch(const std::vector<ToolCall>& calls, int budget);

private:
    Session& session_;
    Run& run_;
    const RunServices& services_;
};

} // namespace dagent::agent
