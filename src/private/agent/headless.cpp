#include "agent/headless.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

#include <signal.h>

#include "agent/agent.hpp"
#include "agent/conversation.hpp"
#include "agent/record.hpp"

namespace dagent::agent {
namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

const char* level_name(Notice::Level level) {
    switch (level) {
    case Notice::Level::info: return "info";
    case Notice::Level::warn: return "warn";
    case Notice::Level::error: return "error";
    }
    return "info";
}

// 末尾不完整的 UTF-8 序列从哪里开始；末尾完整时返回 size。管道读出来的块可能把一个多字节字符切成两半。
std::size_t complete_prefix(std::string_view s) {
    std::size_t i = s.size();
    std::size_t continuation = 0;
    while (i > 0 && continuation < 3 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) {
        --i;
        ++continuation;
    }
    if (i == 0) return s.size();
    const auto lead = static_cast<unsigned char>(s[i - 1]);
    const std::size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return continuation + 1 < need ? i - 1 : s.size();
}

/// @brief text / json 模式的 Sink：stdout 只在结束时写结果，过程进度写 stderr。
/// Sink 会被工具工作线程并发调用（并行组的 ToolOutput），全部访问都在锁内。
struct TextOutput {
    std::mutex mutex;
    std::string step_text;      ///< 当前这一步已显示的正文
    bool step_had_call = false; ///< 当前这一步出现过工具调用（决定中断时要不要补 T1）
    bool waiting = false;       ///< 已发 StepStarted、模型还没有产出
    std::chrono::steady_clock::time_point step_begin{};
    TurnStatus status = TurnStatus::done;
    TurnEnded ended{};

    void operator()(const Event& event) {
        const std::lock_guard lock(mutex);
        std::visit(Overloaded{
                       [&](const TurnStarted&) {},
                       [&](const StepStarted&) {
                           step_text.clear();
                           step_had_call = false;
                           waiting = true;
                           step_begin = std::chrono::steady_clock::now();
                       },
                       [&](const TextDelta& delta) {
                           step_text += delta.text;
                           waiting = false;
                       },
                       [&](const ReasoningDelta&) { waiting = false; },
                       [&](const StreamReset&) {
                           step_text.clear();
                           step_had_call = false;
                           std::cerr << "... discarding partial output from this step\n";
                       },
                       [&](const ToolPending&) {
                           step_had_call = true;
                           waiting = false;
                       },
                       [&](const ToolStarted& started) { std::cerr << "→ " << started.summary << "\n"; },
                       [&](const ToolFinished& finished) {
                           std::cerr << (finished.result.is_error || finished.result.interrupted ? "✗ "
                                                                                                 : "✓ ")
                                     << finished.summary << "\n";
                       },
                       [&](const ToolOutput&) {},
                       [&](const SubEvent&) {}, // 子 Agent 的进度由界面消费；text/json 只报最终结果
                       [&](const Retrying& retrying) {
                           std::cerr << "retry " << retrying.attempt << "/" << retrying.max_attempts << ": "
                                     << retrying.reason << "\n";
                       },
                       [&](const Compacted& compacted) {
                           std::cerr << "compacted: " << compacted.before << " → " << compacted.after
                                     << "\n";
                       },
                       [&](const ContextUpdate&) {},
                       [&](const ModelChanged&) {},
                       [&](const ModeChanged&) {},
                       [&](const Notice& notice) {
                           std::cerr << "[" << level_name(notice.level) << "] " << notice.text << "\n";
                       },
                       [&](const TurnEnded& turn) {
                           ended = turn;
                           status = turn.status;
                           waiting = false;
                           if (!turn.error.empty()) std::cerr << "✗ " << turn.error << "\n";
                       },
                   },
                   event);
    }

    /// 等待模型超过一个 interval 后每个 interval 一行「等待模型… 12s」（docs/design/agent.md §12）。
    void heartbeat() {
        const std::lock_guard lock(mutex);
        if (!waiting) return;
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - step_begin)
                .count();
        if (seconds < 1) return;
        std::cerr << "waiting for the model... " << seconds << "s\n";
    }

    std::string result() {
        const std::lock_guard lock(mutex);
        std::string text = step_text;
        if (status == TurnStatus::interrupted && !step_had_call && !text.empty()) {
            text += std::string(texts::kInterrupted);
        }
        return text;
    }
};

/// @brief 等模型的 Ticker：interval 到点或 jthread 被要求停止就醒，退出时不拖住进程。
std::jthread start_ticker(TextOutput& output, std::chrono::milliseconds interval) {
    return std::jthread([&output, interval](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any cv;
        // jthread 析构时 request_stop：这个回调唤醒等待，退出不拖住进程。
        const std::stop_callback notify(stop, [&cv] { cv.notify_all(); });
        const std::chrono::milliseconds wait = std::max(interval, std::chrono::milliseconds{100});
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            cv.wait_for(lock, wait);
            if (stop.stop_requested()) return;
            output.heartbeat();
        }
    });
}

/// @brief jsonl 模式的 Sink：每个事件一行写 stdout；写失败时停掉这一轮（docs/design/agent.md §12）。
/// tool_output 的块按调用 id 留住末尾不完整的 UTF-8 字节，拼到下一块前面，不在字符中间切开。
struct JsonlOutput {
    std::mutex mutex;
    std::stop_source* stop = nullptr;
    bool failed = false;
    std::map<std::string, std::string> pending; ///< 调用 id → 上一块末尾不完整的字节

    void write_locked(const nlohmann::json& json) {
        if (failed) return;
        std::cout << json.dump() << '\n';
        std::cout.flush();
        if (!std::cout) {
            failed = true;
            stop->request_stop();
        }
    }

    void write_line(const nlohmann::json& json) {
        const std::lock_guard lock(mutex);
        write_locked(json);
    }

    void operator()(const Event& event) {
        const std::lock_guard lock(mutex);
        if (const auto* output = std::get_if<ToolOutput>(&event)) {
            std::string data = std::move(pending[output->id]) + output->chunk;
            const std::size_t cut = complete_prefix(data);
            pending[output->id] = data.substr(cut);
            data.resize(cut);
            if (!data.empty()) write_locked(to_json(ToolOutput{output->id, std::move(data)}));
            return;
        }
        if (const auto* notice = std::get_if<Notice>(&event);
            notice && notice->level != Notice::Level::info) {
            std::cerr << "[" << level_name(notice->level) << "] " << notice->text << '\n';
        }
        if (const auto* finished = std::get_if<ToolFinished>(&event)) {
            // 调用结束时还剩的字节确实不完整，交给 to_json 替换成 U+FFFD。
            if (const auto it = pending.find(finished->id); it != pending.end()) {
                if (!it->second.empty()) write_locked(to_json(ToolOutput{finished->id, it->second}));
                pending.erase(it);
            }
        }
        write_locked(to_json(event));
    }
};

int exit_code(TurnStatus status, bool output_failed) {
    if (status == TurnStatus::done) return output_failed ? 1 : 0;
    // 本轮的 stop 只有两个来源：信号和 jsonl 写失败；后者按失败处理。
    if (status == TurnStatus::interrupted && !output_failed) return 130;
    return 1;
}

} // namespace

Interrupts& install_interrupts() {
    static Interrupts* const state = new Interrupts; // 永不释放：detach 的 sigwait 线程在进程退出前一直引用它
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    ::pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::thread([set] {
        int sig = 0;
        if (::sigwait(&set, &sig) != 0) return;
        if (!state->graceful.load()) std::_Exit(130); // 轮外没有需要收尾的东西
        state->stop.request_stop();
        if (::sigwait(&set, &sig) == 0) std::_Exit(130);
    }).detach();
    return *state;
}

int run_headless(Setup setup, const HeadlessOptions& options, Interrupts& interrupts) {
    const auto heartbeat_interval = setup.options.progress.interval;
    const bool resumed = options.resume_id.has_value() || options.continue_last;

    std::unique_ptr<Agent> agent;
    try {
        if (resumed) {
            const std::optional<std::string_view> prefix = options.resume_id
                                                               ? std::optional<std::string_view>(*options.resume_id)
                                                               : std::nullopt;
            const std::string session_id =
                resolve_session_id(setup.session, setup.cwd, prefix);
            agent = Agent::resume(std::move(setup), session_id, [](const Event&) {});
        } else {
            agent = Agent::create(std::move(setup));
        }
    } catch (const std::exception& error) {
        std::cerr << "startup failed: " << error.what() << "\n";
        return 1;
    }
    const std::string session_id = agent->meta().id;

    const auto begin = std::chrono::steady_clock::now();

    bool output_failed = false;
    TurnStatus status = TurnStatus::done;

    if (options.output == HeadlessOptions::Output::jsonl) {
        JsonlOutput output;
        output.stop = &interrupts.stop;
        output.write_line(
            nlohmann::json{{"type", "session"}, {"id", session_id}, {"resumed", resumed}});
        const Sink sink = [&output](const Event& event) { output(event); };
        const Approver approver{};
        const Asker asker{};
        const TurnContext ctx{sink, approver, asker, interrupts.stop.get_token()};
        interrupts.graceful = true;
        status = agent->run_turn(options.prompt, ctx);
        interrupts.graceful = false;
        const std::lock_guard lock(output.mutex);
        output_failed = output.failed;
    } else {
        TextOutput output;
        const Sink sink = [&output](const Event& event) { output(event); };
        const Approver approver{};
        const Asker asker{};
        const TurnContext ctx{sink, approver, asker, interrupts.stop.get_token()};
        std::jthread ticker = start_ticker(output, heartbeat_interval);
        interrupts.graceful = true;
        status = agent->run_turn(options.prompt, ctx);
        interrupts.graceful = false;
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin)
                .count();

        if (options.output == HeadlessOptions::Output::json) {
            const nlohmann::json object = {
                {"session_id", session_id},
                {"status", std::string(to_string(status))},
                {"error", output.ended.error},
                {"result", output.result()},
                {"steps", output.ended.steps},
                {"tool_calls", output.ended.tool_calls},
                {"usage", {{"prompt", output.ended.total.prompt},
                           {"completion", output.ended.total.completion},
                           {"cached", output.ended.total.cached}}},
                {"duration_ms", ms},
            };
            std::cout << object.dump() << '\n';
            std::cout.flush();
        } else {
            // text 模式的 stdout 只放最终结果；写失败忽略（docs/design/agent.md §12）。
            const std::string result = output.result();
            if (!result.empty()) {
                std::cout << result;
                if (result.back() != '\n') std::cout << '\n';
                std::cout.flush();
            }
        }
    }

    return exit_code(status, output_failed);
}

} // namespace dagent::agent
