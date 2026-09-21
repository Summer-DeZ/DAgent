#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "agent/events.hpp"
#include "mcp/client.hpp"
#include "tools/tools.hpp"

namespace dagent::agent {

struct ServerState {
    std::string name;
    enum class Status { connecting, ready, failed, disconnected, reconnecting };
    Status status = Status::connecting;
    std::size_t tools = 0;
    std::string error;
};

/// 注册表中的 MCP 工具引用 Client，调用方必须在 Hub 析构前销毁这些工具。
class McpHub {
public:
    McpHub(std::vector<mcp::ServerConfig>, mcp::Options);
    ~McpHub();
    McpHub(const McpHub&) = delete;
    McpHub& operator=(const McpHub&) = delete;

    /// 只在 agent 线程、模型请求之间调用：先为断开的 server 启动重连，再等连接中 / 重连中的 server 有结果
    /// （最多 connect_timeout，Notice(info) 提示正在等待），然后合并工具。取消时抛 McpError::cancelled。
    void apply_pending(tools::Registry&, const Sink&, std::stop_token);
    /// @brief 只读快照：把当前已就绪 server 的工具合并进传入的注册表。
    /// 不等待、不重连、不产生通知；供子 Agent 构造时调用一次（线程安全）。
    void snapshot(tools::Registry& registry);
    /// 调度器看到 McpView::disconnected 时调用；返回追加给模型的说明（T12 / T13），不是新断开时返回空。
    std::string mark_disconnected(std::string_view server, std::string reason);
    /// 只交付尚未报告的警告，不刷新或等待连接，适合在一轮结束时调用。
    void report_pending(const Sink&);
    std::vector<ServerState> states() const; ///< 线程安全；ready 表示连接和工具发现完成

private:
    struct Server;
    void connect(Server&);
    void restart_disconnected(tools::Registry&);
    void wait_connecting(const Sink&, std::stop_token);
    bool merge(tools::Registry&, std::stop_token);

    mcp::Options options_;
    mutable std::mutex mutex_;
    std::condition_variable_any settled_; ///< 连接线程得出结果（ready / failed）时通知
    std::vector<Notice> notices_;
    std::vector<std::unique_ptr<Server>> servers_;
};

} // namespace dagent::agent
