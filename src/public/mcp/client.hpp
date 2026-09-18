/// @file client.hpp
/// @brief MCP 客户端：一个 Client 连接一个外部 server，暴露它提供的工具。
///
/// 协议版本以当前 spec（2026-07-28，无握手、每个请求自带 _meta）为准；连接时先发
/// server/discover 探测，server 不认识这个方法（或版本不受支持）时自动退回 2025-11-25 及更早的
/// initialize 握手，因此新旧 server 都能接。第一版只做 tools，不做 resources / prompts /
/// sampling / elicitation / MRTR，也不做 HTTP server 的 OAuth、GET 监听流与 subscriptions/listen。
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "exec/process.hpp"
#include "lib/nlohmann/json.hpp"
#include "net/http.hpp"

namespace dagent::mcp {

enum class Transport {
    stdio, ///< 启动子进程，stdin/stdout 每行一条 JSON-RPC 消息
    http,  ///< Streamable HTTP：每个请求一个 POST，响应可能是 JSON，也可能是 SSE
};

/// @brief 一个 server 的连接参数。对应 .mcp.json 里 "mcpServers" 的一项，由 app 映射并展开
/// ${VAR} 占位符。
struct ServerConfig {
    std::string name;      ///< 工具命名空间前缀；清理成 [A-Za-z0-9_-] 后在一批 server 里唯一，且不含 "__"
                           ///< （跨 server 的冲突本模块检测不到，由 app 保证）
    Transport transport = Transport::stdio;
    std::vector<std::string> command; ///< stdio：完整 argv（命令与参数），argv[0] 按 PATH 查找
    std::vector<std::pair<std::string, std::string>> env; ///< stdio：注入子进程的环境变量
    std::string url;      ///< http：MCP endpoint
    net::Headers headers; ///< http：附加请求头（如 Authorization）
};

/// @brief 暴露给模型的工具。qualified_name 即 mcp__<server>__<name>，是工具名唯一合法的形式
/// （非 [A-Za-z0-9_-] 的字符替换成 '_'）。超过 64 字符、或清理后与同一 server 里前面的工具重名的，
/// 不会出现在 tools() 里（记 warn）。
struct Tool {
    std::string server;         ///< server 名
    std::string name;           ///< server 内的原始工具名
    std::string qualified_name; ///< mcp__<server>__<name>
    std::string description;
    nlohmann::json input_schema;
};

/// @brief 名字清理规则：[A-Za-z0-9_-] 以外的字符换成 '_'。qualified_name 由它拼出；app 检查跨 server
/// 的名字冲突时也用它。
std::string sanitize_name(std::string_view name);

/// @brief tools/call 的结果：content 里的内容块（text / image / resource 等）原样交给核心；
/// 只给结构化结果的 server（没有 content 的）把数据放在 structured 里。
struct CallResult {
    std::vector<nlohmann::json> content;
    nlohmann::json structured; ///< result.structuredContent；没有时为 null
    bool is_error = false;     ///< result.isError；工具执行出错是正常结果，不抛异常
};

/// @brief 客户端选项，对应 config/dagent.json 的 "mcp" 段。
struct Options {
    std::chrono::milliseconds connect_timeout{60000}; ///< 握手与首次 tools/list 每一步请求的超时
                                                      ///< （含 npx/uvx 冷启动）
    std::chrono::milliseconds probe_timeout{10000};   ///< stdio 等 server/discover 回音的上限；
                                                      ///< 超时按经典协议握手（不视为失败）
    exec::Options process; ///< stdio 子进程：环境过滤、kill_grace 等
    /// HTTP 传输：连接超时、响应上限、TLS 校验照用；总时长由每次调用的 timeout 参数控制，
    /// http.timeout 不生效（并发调用各自建连接，不能共用一个整包超时）。
    net::HttpOptions http;
};

/// @brief 传输或协议层面的失败。工具执行失败（isError）属于正常结果，不在这里。
class McpError : public std::runtime_error {
public:
    enum class Kind {
        spawn,        ///< stdio 子进程启动失败（命令不存在、没有执行权限）
        handshake,    ///< 探测/初始化失败（协议版本不受支持、server 拒绝），或任何阶段的 HTTP 401/403
                      ///< （鉴权配置问题）；server 重启后换了协议版本也归这里，需要重新 connect
        timeout,      ///< 等待响应超时；已经对 server 发出 notifications/cancelled（HTTP 现代协议除外）
        cancelled,    ///< stop_token 请求停止；同上
        rpc,          ///< server 返回 JSON-RPC error
        protocol,     ///< 消息不符合协议（坏 JSON、不支持的 resultType、响应体不是 JSON-RPC 的其他 4xx）
        disconnected, ///< 子进程退出、HTTP 连接失败/中断，或 HTTP 5xx
    };

    McpError(Kind kind, const std::string& what) : std::runtime_error(what), kind_(kind) {}
    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

/// @brief 一个 server 的连接。call / refresh_tools 在调用线程上阻塞，可以多线程并发调用：stdio 共用
/// 一个子进程，按 id 分发响应；HTTP 每次调用各建一个连接，互不排队。失败不会影响其他 Client。
class Client {
public:
    /// @brief 启动传输、完成协议协商、拉取一次工具列表。stop 生效或任一步失败时清理连接并抛 McpError。
    static std::unique_ptr<Client> connect(const ServerConfig& config, const Options& opt = {},
                                           std::stop_token stop = {});

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client();

    /// @brief 已发现的工具。refresh_tools() 之后引用失效，核心不要跨刷新保存它。
    const std::vector<Tool>& tools() const;

    /// @brief 重新拉取 tools/list 并替换 tools()。收到 on_tools_changed 通知后由核心线程调用。
    void refresh_tools(std::chrono::milliseconds timeout, std::stop_token stop = {});

    /// @brief 调用工具。tool 传 tools() 里的 qualified_name；args 是参数对象。
    CallResult call(std::string_view tool, const nlohmann::json& args, std::chrono::milliseconds timeout,
                    std::stop_token stop = {});

    /// @brief 注册 tools/list_changed 回调：stdio 在读取线程上触发，HTTP 在调用线程上触发。
    /// 回调里不要阻塞，也不要直接调这个 Client 的方法；更新列表请让核心线程调 refresh_tools。
    /// 现代协议只经 subscriptions/listen 推送这个通知，本模块没有实现，所以只有经典 server 会触发。
    void on_tools_changed(std::function<void()> cb);

    /// @brief 协商出的协议版本（现代为 2026-07-28，经典为 initialize 的结果）。
    const std::string& protocol_version() const;

private:
    Client();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dagent::mcp
