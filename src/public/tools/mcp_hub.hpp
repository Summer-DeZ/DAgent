/// @file mcp_hub.hpp
/// @brief MCP 连接资源：连接/重连、工具目录合并与显式 lease（共享所有权）。
///
/// 主会话在模型步骤边界等待/重连；子会话只在创建时快照。snapshot 注册的工具项
/// 持有 Client 的 shared_ptr，目录刷新或重连不会让仍被子快照引用的连接悬空；
/// 子注册表本身就是这次连接快照的 lease。
#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "agent/events.hpp"
#include "agent/mcp_state.hpp"
#include "mcp/client.hpp"
#include "tools/tools.hpp"

namespace dagent::tools {

class McpHub {
public:
    McpHub(std::vector<mcp::ServerConfig>, mcp::Options);
    ~McpHub();
    McpHub(const McpHub&) = delete;
    McpHub& operator=(const McpHub&) = delete;

    /// 只在 owner 线程、模型请求之间调用：先为断开的 server 启动重连，再等连接中 / 重连中的 server 有结果
    /// （最多 connect_timeout，Notice(info) 提示正在等待），然后合并工具。取消时抛 McpError::cancelled。
    void apply_pending(Registry&, const agent::Sink&, std::stop_token);
    /// @brief 只读快照：把当前已就绪 server 的工具合并进传入的注册表；这些工具项持有连接所有权。
    /// 不等待、不重连、不产生通知；供每个新会话构造时调用一次（线程安全）。
    void snapshot(Registry& registry);
    /// 调度器看到 McpView::disconnected 时调用；返回追加给模型的说明，不是新断开时返回空。
    std::string mark_disconnected(std::string_view server, std::string reason);
    /// 只交付尚未报告的警告，不刷新或等待连接，适合在一轮结束时调用。
    void report_pending(const agent::Sink&);
    std::vector<agent::McpServerState> states() const; ///< 线程安全；ready 表示连接和工具发现完成
    /// @brief 作废连接与所有旧 lease（空表示全部 server）；下次步骤边界按现有 profile 重连。
    void revoke(std::string_view server);

private:
    struct Server;
    void connect(Server&);
    void restart_disconnected(Registry&);
    void wait_connecting(const agent::Sink&, std::stop_token);
    void merge(Registry&, std::stop_token);

    mcp::Options options_;
    mutable std::mutex mutex_;
    std::condition_variable_any settled_; ///< 连接线程得出结果（ready / failed）时通知
    std::vector<agent::Notice> notices_;
    std::vector<std::unique_ptr<Server>> servers_;
};

} // namespace dagent::tools
