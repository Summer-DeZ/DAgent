#include "app/run.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "app/output.hpp"
#include "client/client.hpp"
#include "protocol/dto.hpp"

namespace dagent::app {
namespace {

using namespace std::chrono_literals;

/// @brief 取消当前 Run 所需的协议身份；信号与输出失败都经它请求取消。
struct CancelTarget {
    client::Client* client = nullptr;
    std::string session_id;
    std::uint64_t generation = 0;
};

/// @brief 取当前操作身份后请求取消；两步都异步，调用线程不被 RPC 阻塞。
void request_cancel(const std::shared_ptr<CancelTarget>& target) {
    target->client->call_async(
        "session.snapshot",
        {{"session_id", target->session_id}, {"session_generation", target->generation}},
        [target](std::expected<nlohmann::json, protocol::RpcError> response) {
            if (!response) return;
            const auto operation = response->find("current_operation");
            if (operation == response->end() || !operation->is_object()) return;
            const std::string run_id = operation->value("run_id", "");
            if (run_id.empty()) return;
            target->client->call_async(
                "run.cancel",
                {{"session_id", target->session_id},
                 {"session_generation", target->generation},
                 {"run_id", run_id}},
                [](std::expected<nlohmann::json, protocol::RpcError>) {});
        });
}

/// @brief 等模型的 Ticker：interval 到点或 jthread 被要求停止就醒，退出时不拖住进程。
std::jthread start_ticker(RunOutput& codec, std::chrono::milliseconds interval) {
    return std::jthread([&codec, interval](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any cv;
        const std::stop_callback notify(stop, [&cv] { cv.notify_all(); });
        const std::chrono::milliseconds wait = std::max(interval, std::chrono::milliseconds{100});
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            cv.wait_for(lock, wait);
            if (stop.stop_requested()) return;
            codec.heartbeat();
        }
    });
}

int exit_code(const RunOutput::Summary& summary) {
    if (summary.output_failed) return 1;
    if (summary.status == "done") return 0;
    if (summary.status == "interrupted") return 130;
    return 1;
}

} // namespace

int run_backend(const BackendLaunch& launch, std::string prompt, OutputFormat output,
                Interrupts& interrupts) {
    RunOutput codec(output, std::cout, std::cerr);

    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    bool disconnected = false;
    std::string disconnect_message;
    std::atomic<std::shared_ptr<CancelTarget>> cancel_target;

    client::Client::Callbacks callbacks;
    callbacks.event = [&](const protocol::Event& event) {
        if (!codec.handle(event)) {
            if (const auto target = cancel_target.load()) request_cancel(target);
        }
        if (event.kind != "turn_ended" || event.parent_session_id) return; // 子 Agent 结束不是本 Run 结束
        {
            const std::lock_guard lock(mutex);
            finished = true;
        }
        cv.notify_all();
    };
    callbacks.disconnected = [&](const std::string& message) {
        {
            const std::lock_guard lock(mutex);
            disconnected = true;
            disconnect_message = message;
        }
        cv.notify_all();
    };

    BackendSession session = BackendSession::start(launch, std::move(callbacks));
    const nlohmann::json& initialized = session.initialized();
    const nlohmann::json session_json = initialized.value("session", nlohmann::json::object());
    const std::string session_id = session_json.value("session_id", "");
    const std::uint64_t generation = session_json.value("session_generation", std::uint64_t{0});
    if (session_id.empty()) {
        std::cerr << "startup failed: the backend did not create a session\n";
        return 1;
    }
    codec.set_session(session_id);

    auto target = std::make_shared<CancelTarget>();
    target->client = &session.client();
    target->session_id = session_id;
    target->generation = generation;
    cancel_target.store(std::move(target));
    const std::stop_callback on_signal(interrupts.stop.get_token(), [&cancel_target] {
        if (const auto target = cancel_target.load()) request_cancel(target);
    });

    std::jthread ticker;
    if (output != OutputFormat::jsonl) {
        ticker = start_ticker(codec, std::chrono::milliseconds{initialized.value("progress_interval_ms", 0)});
    }

    const auto begin = std::chrono::steady_clock::now();
    interrupts.graceful = true;
    try {
        session.client().call("input.submit", {{"session_id", session_id},
                                               {"session_generation", generation},
                                               {"text", std::move(prompt)}});
    } catch (const std::exception& error) {
        interrupts.graceful = false;
        if (ticker.joinable()) {
            ticker.request_stop();
            ticker.join();
        }
        std::cerr << "run failed: " << error.what() << "\n";
        return 1;
    }
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return finished || disconnected; });
    }
    interrupts.graceful = false;
    if (ticker.joinable()) {
        ticker.request_stop();
        ticker.join();
    }

    if (disconnected) {
        std::cerr << "run failed: " << disconnect_message << "\n";
        return 1;
    }
    const auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin)
            .count();
    return exit_code(codec.finish(duration_ms));
}

} // namespace dagent::app
