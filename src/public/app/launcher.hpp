/// @file launcher.hpp
/// @brief 前端启动器：创建后端进程、完成 hello/initialize，并在退出时收尾。
///
/// 只有正式前端装配使用；hello 握手 10 秒超时后只清理本前端创建的后端。
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "client/client.hpp"

namespace dagent::app {

struct BackendLaunch {
    std::filesystem::path root, cwd;
    std::vector<std::string> overrides;
    std::string mode = "interactive"; ///< interactive / run / sessions / models
    std::optional<std::string> permissions;
    bool read_only = false;
    bool plan = false;
    std::optional<std::string> resume_id;
    bool continue_last = false;
    std::optional<std::string> log_level;
};

class BackendSession {
public:
    /// @brief 启动后端并完成 hello/initialize；失败抛 std::runtime_error（已回收子进程）。
    static BackendSession start(BackendLaunch launch, client::Client::Callbacks callbacks);

    BackendSession(BackendSession&&) noexcept = default;
    BackendSession& operator=(BackendSession&&) noexcept = default;
    ~BackendSession();
    BackendSession(const BackendSession&) = delete;
    BackendSession& operator=(const BackendSession&) = delete;

    client::Client& client() { return *client_; }
    const nlohmann::json& initialized() const { return initialized_; }

private:
    BackendSession(ipc::BackendProcess process, std::unique_ptr<client::Client> client,
                   nlohmann::json initialized)
        : process_(std::move(process)), client_(std::move(client)),
          initialized_(std::move(initialized)) {}

    ipc::BackendProcess process_;
    std::unique_ptr<client::Client> client_;
    nlohmann::json initialized_;
};

} // namespace dagent::app
