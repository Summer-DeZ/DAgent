/// @file completion_controller.hpp
/// @brief 输入补全控制：命令、技能、文件的来源、筛选、插入与防抖。
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "lib/nlohmann/json.hpp"
#include "ui/overlay/completion.hpp"

namespace dagent::ui {

/// 与弹层同寿，所有操作及 RPC 完成回调都在 UI 线程执行。
/// 作为 Shell 成员使用，RPC 寿命由 Shell 的 CallbackGate 保障。
class CompletionController {
public:
    struct Command {
        std::string alias, title;
    };
    using Done = std::function<void(const nlohmann::json&)>;
    using Request = std::function<void(std::string, nlohmann::json, Done)>;
    struct Actions {
        /// 由宿主经 RPC 寿命门禁转交；失败时沿用原来的静默处理。
        Request request;
        std::function<std::string()> session_id;
        std::function<std::vector<Command>()> commands;
        std::function<void(std::string)> submit;
    };

    CompletionController(tui::Runtime&, PromptBox&, Actions);
    ~CompletionController();
    CompletionController(const CompletionController&) = delete;
    CompletionController& operator=(const CompletionController&) = delete;

    void update();
    void close() { completion_.close(); }
    bool visible() const noexcept { return completion_.visible(); }
    void set_theme(const tui::ThemeTokens& theme) { completion_.set_theme(theme); }

private:
    tui::Runtime& rt_;
    PromptBox& input_;
    Actions actions_;
    std::string kind_;
    tui::TimerId file_debounce_ = 0;
    Completion completion_; ///< 最先析构，关闭回调执行时 kind_ 仍然存活
};

} // namespace dagent::ui
