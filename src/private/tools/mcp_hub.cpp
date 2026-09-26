#include "tools/mcp_hub.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>
#include <utility>

#include "agent/conversation.hpp"

namespace dagent::tools {

struct McpHub::Server {
    mcp::ServerConfig config;
    std::shared_ptr<mcp::Client> client;   // owner 线程操作；snapshot 会跨线程读，指针本身受 mutex_ 保护
    std::shared_ptr<mcp::Client> incoming; // 受 mutex_ 保护
    agent::McpServerState state;           // 受 mutex_ 保护
    bool retried = false;                  // 只在 owner 线程操作
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
    server.connector = std::jthread([this, &server](std::stop_token stop) {
        try {
            std::shared_ptr<mcp::Client> client = mcp::Client::connect(server.config, options_, stop);
            const auto count = client->tools().size();
            std::lock_guard lock(mutex_);
            server.incoming = std::move(client);
            server.state = {server.config.name, agent::McpServerState::Status::ready, count, {}};
        } catch (const std::exception& error) { // 不只 McpError：线程里漏掉的异常会直接 terminate
            if (stop.stop_requested()) return;
            std::lock_guard lock(mutex_);
            server.state = {server.config.name, agent::McpServerState::Status::failed, 0, error.what()};
            notices_.push_back({agent::Notice::Level::warn,
                                std::format("MCP {} failed to connect: {}", server.config.name, error.what())});
        }
        settled_.notify_all();
    });
}

std::vector<agent::McpServerState> McpHub::states() const {
    std::lock_guard lock(mutex_);
    std::vector<agent::McpServerState> states;
    states.reserve(servers_.size());
    for (const auto& server : servers_) states.push_back(server->state);
    return states;
}

void McpHub::report_pending(const agent::Sink& sink) {
    std::vector<agent::Notice> notices;
    {
        std::lock_guard lock(mutex_);
        notices.swap(notices_);
    }
    for (const auto& notice : notices) sink(notice);
}

std::string McpHub::mark_disconnected(std::string_view name, std::string reason) {
    std::lock_guard lock(mutex_);
    for (auto& server : servers_) {
        if (server->state.name != name || server->state.status != agent::McpServerState::Status::ready) continue;
        server->state.status = server->retried ? agent::McpServerState::Status::failed
                                               : agent::McpServerState::Status::disconnected;
        server->state.tools = 0;
        server->state.error = std::move(reason);
        notices_.push_back({agent::Notice::Level::warn,
                            std::format("MCP {} disconnected{}: {}", name,
                                        server->retried ? ", not reconnecting" : ", reconnecting next step",
                                        server->state.error)});
        if (server->retried) return std::format(agent::texts::kMcpUnavailable, name);
        return std::format(agent::texts::kMcpReconnecting, name);
    }
    return {};
}

void McpHub::apply_pending(Registry& registry, const agent::Sink& sink, std::stop_token stop) {
    report_pending(sink);
    restart_disconnected(registry);
    wait_connecting(sink, stop);
    merge(registry, stop);
    report_pending(sink);
}

void McpHub::snapshot(Registry& registry) {
    const std::lock_guard lock(mutex_);
    for (auto& pointer : servers_) {
        Server& server = *pointer;
        if (server.state.status != agent::McpServerState::Status::ready || server.client == nullptr) continue;
        registry.remove_prefix("mcp__" + server.config.name + "__");
        add_mcp(registry, server.client); // 工具项持有 shared_ptr：这次快照的连接寿命由注册表保证
    }
}

void McpHub::restart_disconnected(Registry& registry) {
    for (auto& pointer : servers_) {
        Server& server = *pointer;
        agent::McpServerState::Status status;
        {
            std::lock_guard lock(mutex_);
            status = server.state.status;
        }
        if (status != agent::McpServerState::Status::disconnected &&
            status != agent::McpServerState::Status::failed) {
            continue;
        }
        registry.remove_prefix("mcp__" + server.config.name + "__"); // 必须在 Hub 放弃 Client 之前
        {
            std::lock_guard lock(mutex_);
            server.client.reset(); // 仍被子快照持有的旧连接不受影响
        }
        if (status == agent::McpServerState::Status::disconnected) {
            server.retried = true;
            {
                std::lock_guard lock(mutex_);
                server.state.status = agent::McpServerState::Status::reconnecting;
            }
            connect(server);
        }
    }
}

void McpHub::wait_connecting(const agent::Sink& sink, std::stop_token stop) {
    // 连接中 / 重连中的 server 先等出结果：否则这一步请求里没有它的工具，模型会以为工具不存在。
    const auto pending = [this] {
        return std::ranges::any_of(servers_, [](const auto& server) {
            return server->state.status == agent::McpServerState::Status::connecting ||
                   server->state.status == agent::McpServerState::Status::reconnecting;
        });
    };
    std::unique_lock lock(mutex_);
    if (!pending()) return;
    std::string names;
    for (const auto& server : servers_) {
        if (server->state.status != agent::McpServerState::Status::connecting &&
            server->state.status != agent::McpServerState::Status::reconnecting) {
            continue;
        }
        if (!names.empty()) names += "、";
        names += server->state.name;
    }
    lock.unlock();
    sink(agent::Notice{agent::Notice::Level::info, std::format("Waiting for MCP servers: {}", names)});
    lock.lock();
    // 连接线程自己有超时；这里再以 connect_timeout 兜底，超时就先不带这些工具继续，连上后下一步出现。
    const auto deadline = std::chrono::steady_clock::now() + options_.connect_timeout;
    settled_.wait_until(lock, stop, deadline, [&] { return !pending(); });
    lock.unlock();
    if (stop.stop_requested()) {
        throw mcp::McpError(mcp::McpError::Kind::cancelled, "MCP connection wait interrupted");
    }
}

void McpHub::merge(Registry& registry, std::stop_token stop) {
    for (auto& pointer : servers_) {
        if (stop.stop_requested()) {
            throw mcp::McpError(mcp::McpError::Kind::cancelled, "MCP update interrupted");
        }
        Server& server = *pointer;
        const std::string prefix = "mcp__" + server.config.name + "__";
        std::shared_ptr<mcp::Client> incoming;
        {
            std::lock_guard lock(mutex_);
            incoming = std::move(server.incoming);
        }
        if (incoming) {
            registry.remove_prefix(prefix);
            {
                std::lock_guard lock(mutex_);
                server.client = std::move(incoming);
            }
            add_mcp(registry, server.client);
        }
    }
}

} // namespace dagent::tools
