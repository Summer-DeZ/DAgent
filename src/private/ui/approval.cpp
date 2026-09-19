#include "ui/approval.hpp"

#include <algorithm>
#include <atomic>
#include <future>
#include <utility>

namespace dagent::ui {
namespace {
class FeedbackInput final : public tui::InputBox {
public:
    bool visible = false;
    tui::Size measure(tui::Size available) const override {
        return visible ? tui::InputBox::measure(available) : tui::Size{};
    }
    std::optional<tui::Point> cursor() const override {
        return visible ? tui::InputBox::cursor() : std::nullopt;
    }
};
} // namespace

class ApprovalDialog::Panel final : public tui::Container {
public:
    tui::Scrollback* preview;
    tui::Text *heading, *summary, *choices;
    FeedbackInput* feedback;
    std::unique_ptr<tui::InputBoxHandler> edit;
    std::unique_ptr<tui::ScrollbackMouse> mouse;

    explicit Panel(tui::Runtime& rt) {
        auto add_text = [&] {
            auto widget = std::make_unique<tui::Text>();
            auto* ptr = widget.get(); add({tui::Sizing::content}, std::move(widget)); return ptr;
        };
        heading = add_text(); summary = add_text();
        auto scroll = std::make_unique<tui::Scrollback>(); preview = scroll.get();
        add({tui::Sizing::flex, 1, 2}, std::move(scroll));
        choices = add_text();
        auto input = std::make_unique<FeedbackInput>(); feedback = input.get();
        add({tui::Sizing::content, 0, 0, 6}, std::move(input));
        edit = std::make_unique<tui::InputBoxHandler>(*feedback);
        mouse = std::make_unique<tui::ScrollbackMouse>(rt, *preview);
        rt.bind_mouse(*preview, *mouse);
    }
    tui::Size measure(tui::Size available) const override {
        return {std::min(100, std::max(1, available.cols - 4)),
                std::min(available.rows, std::max(8, available.rows * 3 / 5))};
    }
    void theme(const tui::ThemeTokens& theme) {
        heading->set_theme(theme); heading->set_style(theme.warning);
        summary->set_theme(theme); summary->set_style(theme.text);
        choices->set_theme(theme); choices->set_style(theme.accent);
        preview->set_theme(theme); feedback->set_theme(theme);
    }
};

ApprovalDialog::ApprovalDialog(tui::Runtime& rt, std::function<void()> interrupt)
    : rt_(rt), interrupt_(std::move(interrupt)) {}
ApprovalDialog::~ApprovalDialog() { close(); }

void ApprovalDialog::open(const agent::Approval& approval,
                           std::function<void(agent::Decision)> answer) {
    close(); approval_ = approval; answer_ = std::move(answer);
    auto panel = std::make_unique<Panel>(rt_); panel_ = panel.get();
    panel_->theme(theme_);
    panel_->heading->set_text("── " + approval.reason + " ──");
    panel_->summary->set_text(approval.intent.summary);
    std::string keys = "y 允许   n 拒绝   e 拒绝并说明";
    if (!approval.session_rule.empty()) keys += "\na " + approval.session_rule;
    if (approval.can_network) keys += "\nw 允许并联网";
    keys += "\nEsc 拒绝 · Ctrl+C 中断本轮";
    panel_->choices->set_text(keys);
    auto kind = tui::BlockKind::text;
    std::string preview = approval.intent.preview;
    switch (approval.intent.kind) {
    case tools::Intent::Kind::write: kind = tui::BlockKind::diff; break;
    case tools::Intent::Kind::exec: kind = tui::BlockKind::code; preview = approval.intent.command; break;
    case tools::Intent::Kind::read:
        for (const auto& path : approval.intent.paths) preview += path.path.string() + '\n';
        break;
    case tools::Intent::Kind::external: kind = tui::BlockKind::code; break;
    }
    panel_->preview->document().append_block(kind, preview);
    overlay_ = rt_.open_overlay(std::move(panel), tui::Placement::center, {}, this, panel_->feedback);
}

void ApprovalDialog::close() {
    if (!overlay_) return;
    rt_.unbind_mouse(*panel_->preview);
    rt_.close_overlay(overlay_); overlay_ = 0; panel_ = nullptr; answer_ = {};
}
void ApprovalDialog::set_theme(const tui::ThemeTokens& theme) {
    theme_ = theme; if (panel_) panel_->theme(theme_);
}
void ApprovalDialog::answer(agent::Decision decision) {
    auto callback = std::move(answer_); close(); if (callback) callback(std::move(decision));
}
bool ApprovalDialog::on_event(const tui::Event& e) {
    if (!panel_) return false;
    if (tui::any(e.mods & tui::Mods::ctrl) && e.text == "c") { interrupt_(); return true; }
    if (e.key == tui::Key::page_up || e.key == tui::Key::page_down) {
        panel_->preview->scroll_pages(e.key == tui::Key::page_up ? -1 : 1); return true;
    }
    if (panel_->feedback->visible) {
        if (e.key == tui::Key::escape) {
            panel_->feedback->visible = false; panel_->feedback->invalidate_layout();
        } else if (e.key == tui::Key::enter) {
            answer({agent::Decision::Answer::deny_with_feedback, panel_->feedback->text(), false});
        } else panel_->edit->on_event(e);
        return true;
    }
    if (e.key == tui::Key::escape || e.text == "n") answer({agent::Decision::Answer::deny, {}, false});
    else if (e.text == "y") answer({agent::Decision::Answer::allow, {}, false});
    else if (e.text == "a" && !approval_.session_rule.empty()) answer({agent::Decision::Answer::allow_session, {}, false});
    else if (e.text == "w" && approval_.can_network) answer({agent::Decision::Answer::allow, {}, true});
    else if (e.text == "e") {
        panel_->feedback->visible = true; panel_->feedback->invalidate_layout();
    }
    return true;
}

agent::Decision approve(tui::Runtime& rt, ApprovalDialog& dialog,
                         const agent::Approval& approval, std::stop_token stop) {
    struct Pending { std::promise<agent::Decision> promise; std::atomic<bool> done{false}; };
    auto state = std::make_shared<Pending>();
    auto future = state->promise.get_future();
    auto answer = [state](agent::Decision decision) {
        if (!state->done.exchange(true)) state->promise.set_value(std::move(decision));
    };
    rt.post([&dialog, approval, answer, state] {
        if (!state->done.load()) dialog.open(approval, answer);
    });
    std::stop_callback cancelled(stop, [&] { answer({agent::Decision::Answer::deny, {}, false}); });
    auto result = future.get();
    if (stop.stop_requested()) rt.post([&dialog] { dialog.close(); });
    return result;
}
} // namespace dagent::ui
