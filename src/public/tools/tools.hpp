/// @file tools.hpp
/// @brief 工具层：把外围模块包装成模型能调用的工具——定义名字、说明和参数 Schema，解析校验模型给的
/// 参数，调用外围模块，把结果整理成「给模型的文本」和「给界面与会话的展示数据」。
///
/// prepare 仅声明需求；授权后 prepare_preview 读取并生成预览，写授权后 execute 执行。权限决策、调度、消息历史都不在这里：
/// 工具只陈述自己打算做什么（PreparedIntent 中立摘要），允许/询问/拒绝由核心决定。execute 不抛异常，
/// 一切失败都是 is_error 的 ToolResult。
#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "agent/action.hpp"
#include "agent/grant.hpp"
#include "agent/intent.hpp"
#include "agent/message.hpp"
#include "agent/port_tool.hpp"
#include "agent/tool_data.hpp"
#include "exec/process.hpp"
#include "exec/sandbox.hpp"
#include "exec/srt.hpp"
#include "exec/shell.hpp"
#include "lib/nlohmann/json.hpp"
#include "mcp/client.hpp"
#include "workspace/files.hpp"
#include "workspace/search.hpp"

namespace dagent::tools {

using Spec = agent::ToolSpec;      ///< 工具描述与核心共用一个中立类型
using Grant = agent::ExecutionGrant;
using Result = agent::ToolResult;
using PreparedTool = agent::PreparedTool; ///< 准备完成的普通工具（核心契约）

/// @brief 工具选项，对应 config.json 的 "tools" 段。
struct Options {
    std::map<std::string, exec::Environment> environments;
    std::optional<exec::SrtRuntime> srt;             ///< SRT 后端资源；为空表示使用 Landlock 后端
    std::filesystem::path sandbox_state_root;        ///< 每执行私有目录的父目录（SRT）
    std::size_t max_result_bytes = 0; ///< 每次调用交给模型的文本上限
    std::size_t bash_collect_bytes = 0; ///< 中断时保留的命令输出上限
    int read_default_lines = 0;
    std::size_t read_max_line_bytes = 0;          ///< read 输出里单行的上限，超出截断
    std::size_t grep_max_matches = 0;
    std::size_t glob_max_files = 0;
    std::chrono::milliseconds bash_max_timeout{0}; ///< 模型能要求的最长超时
    std::chrono::milliseconds mcp_call_timeout{0}; ///< 单次 MCP 工具调用的上限
};

/// @brief 会话级状态：核心每个会话建一个，所有调用共用。线程安全。
/// 内部持有 FileTracker：模型读过/写过的文件 → 当时的 Stamp（edit/write 用它做 stale 检测）。
class Context {
public:
    Context(std::filesystem::path root, std::filesystem::path control_root, Options opt,
            workspace::FileOptions files, workspace::SearchOptions search, exec::Options process);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    const std::filesystem::path& root() const; ///< 工作区根，即工具路径参数的基准
    const std::filesystem::path& control_root() const; ///< 代理控制面根（配置/数据/日志/运行）
    /// 内容是否受保护：敏感名、控制面、.git 内部或工作区外；这些路径的内容必须先批准再读取。
    bool content_restricted(const std::filesystem::path& path, bool inside_workspace) const;
    bool can_read(const Grant&, const std::filesystem::path&) const;
    void require_access(const Grant&, const workspace::Resolved&, agent::Access) const;
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

/// @brief 一个普通工具：固定的 Spec + 把参数变成 PreparedTool 的 prepare。
class Tool {
public:
    virtual ~Tool() = default;
    virtual const Spec& spec() const = 0;
    /// @brief 解析参数与路径元数据；内容读取必须等待授权后的 preview/execute。
    virtual std::expected<std::unique_ptr<PreparedTool>, Result> prepare(
        std::string_view arguments, Context&) const = 0;
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

/// @brief 实现核心 ToolSession 端口：注册表 + 会话 Context（核心只看描述与准备结果）。
class ToolSession final : public agent::ToolSession {
public:
    ToolSession(const Registry& registry, Context& context);

    std::vector<agent::ToolSpec> specs() const override;
    std::expected<std::unique_ptr<agent::PreparedTool>, agent::ToolResult> prepare(
        std::string_view name, std::string_view arguments) const override;

private:
    const Registry& registry_;
    Context& context_;
};

/// @brief 注册内置工具：read / write / edit / bash / grep / glob。
void add_builtin(Registry&);

/// @brief 把一个 MCP server 的所有工具包进 Registry（名字 mcp__<server>__<name>）。
/// 工具项持有 Client 的 shared_ptr：刷新/重连替换目录时，仍被子快照引用的连接不会悬空。
void add_mcp(Registry&, std::shared_ptr<mcp::Client>);

/// @brief 把解析后的路径转成权限决策用的中立资源意图（tools 适配层的公共转换）。
agent::ResourceIntent to_intent(const workspace::Resolved&, agent::Access);

} // namespace dagent::tools
