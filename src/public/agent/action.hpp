/// @file action.hpp
/// @brief 动作契约：一次已准备普通工具的身份、只读意图与执行入口。
///
/// 身份在 prepare 构造时固定（不再有运行中改写）；execute 只接收决定好的授权、输出接收器和取消。
/// shell 分析树、MCP Client、workspace 实现对象留在具体 PreparedTool 内部（architecture-refactor §5.1）。
#pragma once

#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include "agent/grant.hpp"
#include "agent/identity.hpp"
#include "agent/intent.hpp"
#include "agent/tool_data.hpp"

namespace dagent::agent {

/// @brief 一次动作的身份：模型调用 ID（持久）与后端内部 invocation 身份。
struct InvocationContext {
    InvocationId invocation_id; ///< 本后端内区分动作；不落库
    std::string call_id;        ///< 模型返回的调用 ID；记录与事件沿用
};

/// @brief 准备完成的普通工具：构造时拥有身份与参数/分析，公开只读 intent。
class PreparedTool {
public:
    virtual ~PreparedTool() = default;
    PreparedTool(const PreparedTool&) = delete;
    PreparedTool& operator=(const PreparedTool&) = delete;

    const PreparedIntent& intent() const { return intent_; }
    const InvocationId& invocation_id() const { return invocation_id_; }
    const std::string& call_id() const { return call_id_; }

    /// @brief 在调用线程上阻塞执行；不抛异常：取消返回 interrupted，环境失败返回 is_error 结果。
    ToolResult execute(const ExecutionGrant& grant,
                       const std::function<void(std::string_view)>& on_output, std::stop_token stop);

protected:
    explicit PreparedTool(InvocationContext context)
        : invocation_id_(std::move(context.invocation_id)), call_id_(std::move(context.call_id)) {}

    PreparedIntent intent_; ///< 由各实现的 prepare 填好（中立摘要）

private:
    virtual ToolResult do_execute(const ExecutionGrant&, const std::function<void(std::string_view)>&,
                                  std::stop_token) = 0;

    InvocationId invocation_id_;
    std::string call_id_;
};

} // namespace dagent::agent
