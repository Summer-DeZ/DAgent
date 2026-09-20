#include "agent/mcp_hub.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <thread>
#include <utility>

#include "agent/conversation.hpp"

namespace dagent::agent {

struct McpHub::Server {
    mcp::ServerConfig config;
    // 回调只访问此标志；它必须比两个 Client 活得久。
    std::atomic<bool> tools_changed{false};
    std::unique_ptr<mcp::Client> client;   // 只在 agent 线程操作
    std::unique_ptr<mcp::Client> incoming; // 受 mutex_ 保护
    ServerState state;                    // 受 mutex_ 保护
    bool retried = false;                 // 只在 agent 线程操作
    std::jthread connector;
};

McpHub::McpHub(std::vector<mcp::ServerConfig> configs, mcp::Options options)
    : options_(std::move(options)) {
    for (auto& config : configs) {
        auto server = std::make_unique<Server>();
        server->state.name = config.name;
        server->config = std::move(config);
        servers_.push_back(std::move(server));
    }
    for (auto& server : servers_) connect(*server);
}

McpHub::~McpHub() {
    for (auto& server : servers_) server->connector.request_stop();
    for (auto& server : servers_) {
        if (server->connector.joinable()) server->connector.join();
    }
}

void McpHub::connect(Server& server) {
    server.tools_changed = false;
    server.connector = std::jthread([this, &server](std::stop_token stop) {
        try {
            auto client = mcp::Client::connect(server.config, options_, stop);
            client->on_tools_changed([&server] { server.tools_changed = true; });
            const auto count = client->tools().size();
            std::lock_guard lock(mutex_);
            server.incoming = std::move(client);
            server.state = {server.config.name, ServerState::Status::ready, count, {}};
        } catch (const std::exception& error) { // 不只 McpError：线程里漏掉的异常会直接 terminate
            if (stop.stop_requested()) return;
            std::lock_guard lock(mutex_);
            server.state = {server.config.name, ServerState::Status::failed, 0, error.what()};
            notices_.push_back({Notice::Level::warn,
                                std::format("MCP {} failed to connect: {}", server.config.name, error.what())});
        }
        settled_.notify_all();
    });
}

std::vector<ServerState> McpHub::states() const {
    std::lock_guard lock(mutex_);
    std::vector<ServerState> states;
    states.reserve(servers_.size());
    for (const auto& server : servers_) states.push_back(server->state);
    return states;
}

void McpHub::report_pending(const Sink& sink) {
    std::vector<Notice> notices;
    {
        std::lock_guard lock(mutex_);
        notices.swap(notices_);
    }
    for (const auto& notice : notices) sink(notice);
}

std::string McpHub::mark_disconnected(std::string_view name, std::string reason) {
    std::lock_guard lock(mutex_);
    for (auto& server : servers_) {
        if (server->state.name != name || server->state.status != ServerState::Status::ready) continue;
        server->state.status = server->retried ? ServerState::Status::failed
                                              : ServerState::Status::disconnected;
        server->state.tools = 0;
        server->state.error = std::move(reason);
        notices_.push_back({Notice::Level::warn,
                            std::format("MCP {} disconnected{}: {}", name,
                                        server->retried ? ", not reconnecting" : ", reconnecting next step",
                                        server->state.error)});
        if (server->retried) return std::format(texts::kMcpUnavailable, name);
        return std::format(texts::kMcpReconnecting, name);
    }
    return {};
}

void McpHub::apply_pending(tools::Registry& registry, const Sink& sink, std::stop_token stop) {
    report_pending(sink);
    // 刷新失败会把 server 标成断开，要再走一遍重连；每个 server 至多断开两次（第二次直接 failed），循环有界。
    do {
        restart_disconnected(registry);
        wait_connecting(sink, stop);
    } while (merge(registry, stop));
    report_pending(sink);
}

void McpHub::restart_disconnected(tools::Registry& registry) {
    for (auto& pointer : servers_) {
        Server& server = *pointer;
        ServerState::Status status;
        {
            std::lock_guard lock(mutex_);
            status = server.state.status;
        }
        if (status != ServerState::Status::disconnected && status != ServerState::Status::failed) continue;
        registry.remove_prefix("mcp__" + server.config.name + "__"); // 必须在 Client 析构之前
        server.client.reset();
        if (status == ServerState::Status::disconnected) {
            server.retried = true;
            {
                std::lock_guard lock(mutex_);
                server.state.status = ServerState::Status::reconnecting;
            }
            connect(server);
        }
    }
}

void McpHub::wait_connecting(const Sink& sink, std::stop_token stop) {
    // 连接中 / 重连中的 server 先等出结果：否则这一步请求里没有它的工具，模型会以为工具不存在。
    const auto pending = [this] {
        return std::ranges::any_of(servers_, [](const auto& server) {
            return server->state.status == ServerState::Status::connecting ||
                   server->state.status == ServerState::Status::reconnecting;
        });
    };
    std::unique_lock lock(mutex_);
    if (!pending()) return;
    std::string names;
    for (const auto& server : servers_) {
        if (server->state.status != ServerState::Status::connecting &&
            server->state.status != ServerState::Status::reconnecting) continue;
        if (!names.empty()) names += "、";
        names += server->state.name;
    }
    lock.unlock();
    sink(Notice{Notice::Level::info, std::format("Waiting for MCP servers: {}", names)});
    lock.lock();
    // 连接线程自己有超时；这里再以 connect_timeout 兜底，超时就先不带这些工具继续，连上后下一步出现。
    const auto deadline = std::chrono::steady_clock::now() + options_.connect_timeout;
    settled_.wait_until(lock, stop, deadline, [&] { return !pending(); });
    lock.unlock();
    if (stop.stop_requested()) throw mcp::McpError(mcp::McpError::Kind::cancelled, "MCP connection wait interrupted");
}

bool McpHub::merge(tools::Registry& registry, std::stop_token stop) {
    bool changed = false;
    for (auto& pointer : servers_) {
        if (stop.stop_requested()) {
            throw mcp::McpError(mcp::McpError::Kind::cancelled, "MCP update interrupted");
        }
        Server& server = *pointer;
        const std::string prefix = "mcp__" + server.config.name + "__";
        std::unique_ptr<mcp::Client> incoming;
        {
            std::lock_guard lock(mutex_);
            incoming = std::move(server.incoming);
        }
        if (incoming) {
            registry.remove_prefix(prefix);
            server.client = std::move(incoming);
            tools::add_mcp(registry, *server.client);
        }
        if (!server.client || !server.tools_changed.exchange(false)) continue;
        try {
            server.client->refresh_tools(options_.connect_timeout, stop);
            registry.remove_prefix(prefix);
            tools::add_mcp(registry, *server.client);
            std::lock_guard lock(mutex_);
            server.state.tools = server.client->tools().size();
        } catch (const mcp::McpError& error) {
            if (error.kind() == mcp::McpError::Kind::cancelled) {
                server.tools_changed = true;
                throw;
            }
            changed |= !mark_disconnected(server.config.name, error.what()).empty();
        }
    }
    return changed;
}

} // namespace dagent::agent
