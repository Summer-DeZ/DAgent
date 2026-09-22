/// @file tools.hpp
/// @brief 工具层：把外围模块包装成模型能调用的工具——定义名字、说明和参数 Schema，解析校验模型给的
/// 参数，调用外围模块，把结果整理成「给模型的文本」和「给界面与会话的展示数据」。
///
/// 两阶段：prepare 解析校验预演（无副作用），run 真正执行。权限决策、调度、消息历史都不在这里：
/// 工具只陈述自己打算做什么（PreparedIntent 中立摘要），允许/询问/拒绝由核心决定。run 不抛异常，
/// 一切失败都是 is_error 的 ToolResult。
#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "agent/grant.hpp"
#include "agent/intent.hpp"
#include "agent/message.hpp"
#include "agent/tool_data.hpp"
#include "exec/process.hpp"
#include "exec/sandbox.hpp"
#include "exec/shell.hpp"
#include "lib/nlohmann/json.hpp"
#include "mcp/client.hpp"
#include "workspace/files.hpp"
#include "workspace/search.hpp"

namespace dagent::tools {

using Spec = agent::ToolSpec;      ///< 工具描述与核心共用一个中立类型
using Grant = agent::ExecutionGrant;
using Result = agent::ToolResult;

/// @brief 工具选项，对应 config/dagent.json 的 "tools" 段。
struct Options {
    std::size_t max_result_bytes = 32 << 10;         ///< 每次调用交给模型的文本上限（约 8k token）
    int read_default_lines = 2000;
    std::size_t read_max_line_bytes = 2000;          ///< read 输出里单行的上限，超出截断
    std::size_t grep_max_matches = 200;
    std::size_t glob_max_files = 200;
    std::chrono::milliseconds bash_max_timeout{600000}; ///< 模型能要求的最长超时
    std::chrono::milliseconds mcp_call_timeout{120000}; ///< 单次 MCP 工具调用的上限
};

/// @brief 会话级状态：核心每个会话建一个，所有调用共用。线程安全。
/// 内部持有 FileTracker：模型读过/写过的文件 → 当时的 Stamp（edit/write 用它做 stale 检测）。
class Context {
public:
    Context(std::filesystem::path root, Options opt, workspace::FileOptions files,
            workspace::SearchOptions search, exec::Options process);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    const std::filesystem::path& root() const; ///< 工作区根，即工具路径参数的基准
    const Options& options() const;
    const workspace::FileOptions& files() const;
    const workspace::SearchOptions& search() const;
    const exec::Options& process() const;

    /// FileTracker 按 resolve 之后的路径做键（./a、a、指向同一文件的符号链接算同一个文件）。
    std::optional<workspace::Stamp> tracked_stamp(const workspace::Resolved&) const;
    void track(const workspace::Resolved&, workspace::Stamp);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief 一次已经通过 prepare 的调用。核心按 PreparedIntent 做完权限决策后执行。
class Call {
public:
    virtual ~Call() = default;

    /// @brief 决定权限所需的中立摘要；底层分析细节（分析树、Client 指针）不在这里。
    const agent::PreparedIntent& intent() const { return intent_; }

    /// @brief 核心在 prepare 之后写入本调用的 id；需要把子事件挂回父会话的工具用它。
    void set_call_id(std::string id) { call_id_ = std::move(id); }
    const std::string& call_id() const { return call_id_; }

    /// @brief 在调用线程上阻塞执行；on_output 只有 bash 会调用（原始输出块）。
    /// 不抛异常：取消返回 interrupted=true，其他失败（包括各种环境问题）都是 is_error 的 Result。
    Result run(const Grant& grant, const std::function<void(std::string_view)>& on_output,
               std::stop_token stop);

protected:
    agent::PreparedIntent intent_; ///< 由各实现的 prepare 填好（中立摘要）

private:
    virtual Result do_run(const Grant&, const std::function<void(std::string_view)>&,
                          std::stop_token) = 0;

    std::string call_id_;
};

/// @brief 一个工具：固定的 Spec + 把参数变成 Call 的 prepare。
class Tool {
public:
    virtual ~Tool() = default;
    virtual const Spec& spec() const = 0;
    /// @brief 解析、校验、预演。参数有问题时返回 is_error 的 Result。没有副作用（可以读文件）。
    virtual std::expected<std::unique_ptr<Call>, Result> prepare(std::string_view arguments,
                                                                 Context&) const = 0;
};

/// @brief 名字到工具的映射，specs() 按注册顺序返回（保证每次请求里工具列表顺序稳定）。
class Registry {
public:
    void add(std::unique_ptr<Tool>);             ///< 同名替换，保持原有位置
    void remove_prefix(std::string_view prefix); ///< 刷新某个 MCP server 前移除 mcp__<server>__
    void retain(const std::vector<std::string>& names); ///< 只保留列表中的工具（子 Agent 工具集收窄）
    const Tool* find(std::string_view name) const;
    std::vector<const Spec*> specs() const;

private:
    std::vector<std::unique_ptr<Tool>> tools_;
};

/// @brief 注册内置工具：read / write / edit / bash / grep / glob。
void add_builtin(Registry&);

/// @brief 把一个 MCP server 的所有工具包进 Registry（名字 mcp__<server>__<name>）。
/// Client 要比这些工具活得久；refresh_tools 之后核心先 remove_prefix 再重新 add_mcp。
void add_mcp(Registry&, mcp::Client&);

/// @brief 把解析后的路径转成权限决策用的中立资源意图（tools 适配层的公共转换）。
agent::ResourceIntent to_intent(const workspace::Resolved&, agent::Access);

} // namespace dagent::tools
