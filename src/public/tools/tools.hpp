/// @file tools.hpp
/// @brief 工具层：把外围模块包装成模型能调用的工具——定义名字、说明和参数 Schema，解析校验模型给的
/// 参数，调用外围模块，把结果整理成「给模型的文本」和「给界面与会话的 View」。
///
/// 两阶段：prepare 解析校验预演（无副作用），run 真正执行。权限决策、调度、消息历史都不在这里：
/// 工具只陈述自己打算做什么（Intent），允许/询问/拒绝由核心决定。run 不抛异常，一切失败都是
/// is_error 的 Result。
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

#include "exec/process.hpp"
#include "exec/sandbox.hpp"
#include "lib/nlohmann/json.hpp"
#include "mcp/client.hpp"
#include "tools/view.hpp"
#include "workspace/files.hpp"
#include "workspace/search.hpp"

namespace dagent::tools {

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

/// @brief 给模型的工具描述。核心把它转成 agent::ToolDef（字段一一对应）。
struct Spec {
    std::string name, description;
    nlohmann::json parameters = nlohmann::json::object(); ///< JSON Schema
};

/// @brief 工具打算做什么：权限决策的输入，只描述，不决策。
struct Intent {
    enum class Kind { read, write, exec, external }; ///< external：MCP 工具，语义未知
    Kind kind = Kind::read;
    std::vector<workspace::Resolved> paths; ///< read/write 涉及的路径，带 inside_workspace
    std::string command;                    ///< exec：原始命令
    bool known_readonly = false;            ///< exec：exec::is_known_readonly 的结果
    std::string preview;                    ///< write/edit：unified diff，给确认对话框
    std::string summary;                    ///< 一行描述，如「编辑 src/a.cpp（+3 −1）」
};

/// @brief 核心的决定，执行时传回。
struct Grant {
    exec::Mode sandbox = exec::Mode::workspace_write; ///< 只对 bash 有意义
    bool allow_network = false;
};

/// @brief 一次工具调用的结果。
struct Result {
    std::string text;         ///< 给模型：合法 UTF-8，已按 max_result_bytes 截断
    bool is_error = false;    ///< 模型视角的失败：参数错、找不到、匹配失败、退出码非 0……
    bool interrupted = false; ///< stop_token 触发；text 里是已有的部分输出
    View display;             ///< 给界面与会话，见 tools/view.hpp
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

/// @brief 一次已经通过 prepare 的调用。核心按 Intent 做完权限决策后执行。
class Call {
public:
    virtual ~Call() = default;

    const Intent& intent() const { return intent_; }

    /// @brief 在调用线程上阻塞执行；on_output 只有 bash 会调用（原始输出块）。
    /// 不抛异常：取消返回 interrupted=true，其他失败（包括各种环境问题）都是 is_error 的 Result。
    Result run(const Grant& grant, const std::function<void(std::string_view)>& on_output,
               std::stop_token stop);

protected:
    Intent intent_; ///< 由各实现的 prepare 填好

private:
    virtual Result do_run(const Grant&, const std::function<void(std::string_view)>&,
                          std::stop_token) = 0;
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

} // namespace dagent::tools
