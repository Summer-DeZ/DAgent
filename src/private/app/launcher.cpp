#include "app/launcher.hpp"

#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <utility>

#include "protocol/dto.hpp"
#include "protocol/rpc.hpp"

#ifndef DAGENT_VERSION
#define DAGENT_VERSION "dev"
#endif

namespace dagent::app {
namespace {

using namespace std::chrono_literals;

/// @brief 带超时的同步请求（hello 握手用）。
nlohmann::json call_with_timeout(client::Client& client, const std::string& method,
                                 nlohmann::json params, std::chrono::milliseconds timeout) {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::optional<nlohmann::json> result;
    std::optional<protocol::RpcError> error;
    client.call_async(method, std::move(params),
                      [&](std::expected<nlohmann::json, protocol::RpcError> response) {
                          {
                              const std::lock_guard lock(mutex);
                              if (response) result = std::move(*response);
                              else error = std::move(response.error());
                              done = true;
                          }
                          cv.notify_all();
                      });
    std::unique_lock lock(mutex);
    if (!cv.wait_for(lock, timeout, [&] { return done; })) {
        throw std::runtime_error(method + " timed out");
    }
    if (error) throw client::RpcFailure(*error);
    return std::move(*result);
}

} // namespace

BackendSession BackendSession::start(BackendLaunch launch, client::Client::Callbacks callbacks) {
    std::string error;
    std::optional<ipc::BackendProcess> process = ipc::spawn_backend(error);
    if (!process) throw std::runtime_error(error);
    auto client = std::make_unique<client::Client>(std::move(process->channel), std::move(callbacks));

    try {
        nlohmann::json hello = call_with_timeout(
            *client, "backend.hello",
            {{"protocol_version", {{"major", protocol::kMajor}, {"minor", protocol::kMinor}}},
             {"frontend_version", DAGENT_VERSION}},
            10s);
        (void)hello;
        nlohmann::json initialize{
            {"root", launch.root.string()},
            {"cwd", launch.cwd.string()},
            {"mode", launch.mode},
            {"ordered_overrides", launch.overrides},
            {"read_only", launch.read_only},
            {"plan", launch.plan},
            {"continue_last", launch.continue_last},
        };
        if (launch.permissions) initialize["permissions"] = *launch.permissions;
        if (launch.resume_id) initialize["resume_id"] = *launch.resume_id;
        if (launch.log_level) initialize["log_level"] = *launch.log_level;
        nlohmann::json initialized = client->call("app.initialize", std::move(initialize));
        ipc::BackendProcess moved = std::move(*process);
        return BackendSession(std::move(moved), std::move(client), std::move(initialized));
    } catch (...) {
        client->close();
        ipc::wait_or_terminate(*process, 5s);
        throw;
    }
}

BackendSession::~BackendSession() {
    if (client_) {
        if (client_->connected()) {
            try {
                client_->call("backend.shutdown");
            } catch (const std::exception&) {
                // 后端已在收尾或已退出；宽限回收由 wait_or_terminate 负责。
            }
        }
        client_->close();
        client_.reset();
    }
    ipc::wait_or_terminate(process_, 10s);
}

} // namespace dagent::app
