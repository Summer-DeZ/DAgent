/// @file turn_runner.hpp
/// @brief TurnRunner：阻塞式核心循环算法；无跨会话状态，只通过 Session/Run/RunServices 工作。
///
/// 一轮的结束只经 finish 一个入口；循环内不出现 UI、SQLite、HTTP 或具体 MCP 类型。
#pragma once

#include <string>

#include "agent/run.hpp"
#include "agent/run_services.hpp"
#include "agent/session.hpp"

namespace dagent::agent {

class TurnRunner {
public:
    /// @brief 执行一轮普通对话（模型 → 工具 → 回填），直到 Run 收尾。
    RunOutcome run(Session&, Run&, const RunServices&, std::string input);
    /// @brief 手动压缩：不追加 user/turn_end，使用同一收尾约束。
    RunOutcome compact(Session&, Run&, const RunServices&);

private:
    RunOutcome finish(Session&, Run&, const RunServices&, TurnStatus, std::string error);
};

} // namespace dagent::agent
