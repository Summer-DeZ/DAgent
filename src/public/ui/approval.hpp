#pragma once

#include <functional>
#include <memory>
#include "agent/events.hpp"
#include "tui/runtime.hpp"

namespace dagent::ui {
/// 渲染线程的模态处理器。等待/取消通过 approve 桥接，控件从不跨线程访问。
class ApprovalDialog final : public tui::EventHandler {
public:
    ApprovalDialog(tui::Runtime&, std::function<void()> interrupt);
    ~ApprovalDialog() override;
    void open(const agent::Approval&, std::function<void(agent::Decision)> answer);
    void open(const agent::Question&, std::function<void(agent::Answer)> answer);
    void close();
    bool active() const { return overlay_ != 0; }
    void set_theme(const tui::ThemeTokens&);
    bool on_event(const tui::Event&) override;
private:
    class Panel;
    void answer(agent::Decision);
    void answer(agent::Answer);
    void refresh_question();
    tui::Runtime& rt_;
    std::function<void()> interrupt_;
    std::function<void(agent::Decision)> answer_;
    std::function<void(agent::Answer)> question_answer_;
    agent::Approval approval_;
    agent::Question question_;
    std::vector<bool> selected_;
    int question_cursor_ = 0;
    uint32_t overlay_ = 0;
    Panel* panel_ = nullptr;
    tui::ThemeTokens theme_ = tui::dark_theme();
};

/// agent 线程调用；stop_callback 直接完成 promise，不依赖渲染线程及时处理取消。
agent::Decision approve(tui::Runtime&, ApprovalDialog&, const agent::Approval&, std::stop_token);
agent::Answer ask(tui::Runtime&, ApprovalDialog&, const agent::Question&, std::stop_token);
} // namespace dagent::ui
