#include "agent/headless.hpp"

#include <iostream>
#include <mutex>
#include <string>
#include <variant>

#include "agent/agent.hpp"
#include "agent/conversation.hpp"

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

/// @brief text 模式的 Sink：stdout 只在结束时写最终回复，过程进度写 stderr。
struct TextOutput {
    std::mutex mutex;
    std::string step_text;      ///< 当前这一步已显示的正文
    bool step_had_call = false; ///< 当前这一步出现过工具调用（决定中断时要不要补 T1）
    TurnStatus status = TurnStatus::done;

    void operator()(const Event& event) {
        const std::lock_guard lock(mutex);
        std::visit(Overloaded{
                       [&](const TurnStarted&) {},
                       [&](const StepStarted&) {
                           step_text.clear();
                           step_had_call = false;
                       },
                       [&](const TextDelta& delta) { step_text += delta.text; },
                       [&](const ReasoningDelta&) {},
                       [&](const StreamReset&) {
                           step_text.clear();
                           step_had_call = false;
                           std::cerr << "… 丢弃这一步已显示的部分输出\n";
                       },
                       [&](const ToolPending&) { step_had_call = true; },
                       [&](const ToolStarted& started) {
                           std::cerr << "→ " << started.summary << "\n";
                       },
                       [&](const ToolFinished& finished) {
                           std::cerr << (finished.result.is_error || finished.result.interrupted ? "✗ "
                                                                                                : "✓ ")
                                     << finished.summary << "\n";
                       },
                       [&](const ToolOutput&) {},
                       [&](const Retrying& retrying) {
                           std::cerr << "重试 " << retrying.attempt << "/" << retrying.max_attempts << "："
                                     << retrying.reason << "\n";
                       },
                       [&](const Compacted& compacted) {
                           std::cerr << "上下文已压缩：" << compacted.before << " → " << compacted.after
                                     << "\n";
                       },
                       [&](const ContextUpdate&) {},
                       [&](const Notice& notice) {
                           std::cerr << "[" << level_name(notice.level) << "] " << notice.text << "\n";
                       },
                       [&](const TurnEnded& ended) {
                           status = ended.status;
                           if (!ended.error.empty()) std::cerr << "✗ " << ended.error << "\n";
                       },
                   },
                   event);
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

} // namespace

int run_headless(Setup setup, const HeadlessOptions& options) {
    if (options.output != HeadlessOptions::Output::text) {
        std::cerr << "run 模式目前只支持 --output text\n";
        return 2;
    }
    if (options.resume_id || options.continue_last) {
        std::cerr << "会话恢复尚未实现\n";
        return 2;
    }

    std::unique_ptr<Agent> agent;
    try {
        agent = Agent::create(std::move(setup));
    } catch (const std::exception& error) {
        std::cerr << "启动失败：" << error.what() << "\n";
        return 1;
    }

    TextOutput output;
    std::stop_source stop;
    const TurnStatus status =
        agent->run_turn(options.prompt, [&output](const Event& event) { output(event); }, Approver{},
                        stop.get_token());

    const std::string result = output.result();
    if (!result.empty()) {
        std::cout << result;
        if (result.back() != '\n') std::cout << '\n';
        std::cout.flush();
    }
    return status == TurnStatus::done ? 0 : 1;
}

} // namespace dagent::agent
