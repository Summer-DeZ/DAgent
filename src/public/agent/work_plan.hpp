/// @file work_plan.hpp
/// @brief 当前会话的整份计划：todo 每次完整替换，不做增量合并（B21）。
///
/// 持久来源仍是 todo 工具结果的 view（记录路线 L13）；这里是会话内存状态，显示投影由前端生成。
#pragma once

#include <utility>

#include "agent/tool_data.hpp"

namespace dagent::agent {

class WorkPlan {
public:
    void replace(TodoView plan) { plan_ = std::move(plan); }
    const TodoView& view() const { return plan_; }

private:
    TodoView plan_;
};

} // namespace dagent::agent
