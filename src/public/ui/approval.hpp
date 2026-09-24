#pragma once

#include <functional>
#include <memory>

#include "tui/runtime.hpp"
#include "ui/projection.hpp"

namespace dagent::ui {
/// 渲染线程的模态处理器。回答只提交用户决定，由前端经 interaction.answer 送出。
class ApprovalDialog final : public tui::EventHandler {
public:
    ApprovalDialog(tui::Runtime&, std::function<void()> interrupt);
    ~ApprovalDialog() override;
    void open(const ApprovalRequest&, std::function<void(ApprovalAnswer)> answer);
    void open(const QuestionRequest&, std::function<void(QuestionAnswer)> answer);
    void close();
    bool active() const { return overlay_ != 0; }
    void set_theme(const tui::ThemeTokens&);
    bool on_event(const tui::Event&) override;
private:
    class Panel;
    void answer(ApprovalAnswer);
    void answer(QuestionAnswer);
    void refresh_question();
    tui::Runtime& rt_;
    std::function<void()> interrupt_;
    std::function<void(ApprovalAnswer)> answer_;
    std::function<void(QuestionAnswer)> question_answer_;
    ApprovalRequest approval_;
    QuestionRequest question_;
    std::vector<bool> selected_;
    int question_cursor_ = 0;
    uint32_t overlay_ = 0;
    Panel* panel_ = nullptr;
    tui::ThemeTokens theme_ = tui::dark_theme();
};

} // namespace dagent::ui
