#include "ui/approval.hpp"
#include "ui/strings.hpp"
#include "ui/display.hpp"

#include <algorithm>
#include <atomic>
#include <format>
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
    const tui::ThemeTokens* current_theme = &tui::dark_theme();
    std::string tool;
    bool question = false;

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
        // 为底边提示保留一行，不能覆盖审批选项或反馈输入。
        add({tui::Sizing::fixed, 1}, std::make_unique<tui::Text>());
        edit = std::make_unique<tui::InputBoxHandler>(*feedback);
        mouse = std::make_unique<tui::ScrollbackMouse>(rt, *preview);
        rt.bind_mouse(*preview, *mouse);
    }
    tui::Size measure(tui::Size available) const override {
        return {std::min(100, std::max(1, available.cols - 4)),
                std::min(available.rows, std::max(8, available.rows * 3 / 5))};
    }
    void layout(tui::Rect area) override {
        tui::Container::layout(area);
        // 预览和输入不能借用边框列；长权限说明在预览区自然折行。
        for (tui::Widget* child : {static_cast<tui::Widget*>(preview),
                                   static_cast<tui::Widget*>(feedback)}) {
            const auto r = child->rect();
            child->layout({2, r.y, std::max(0, area.w - 4), r.h});
        }
    }
    void render(tui::Surface& surface) override {
        surface.fill({0, 0, surface.cols(), surface.rows()}, U' ', current_theme->background_panel);
        tui::Container::render(surface);
        const int w = surface.cols(), h = surface.rows();
        if (w < 4 || h < 3) return;
        surface.fill({1, 0, w - 2, 1}, U'─', current_theme->border_active);
        surface.fill({1, h - 1, w - 2, 1}, U'─', current_theme->border_active);
        surface.fill({0, 1, 1, h - 2}, U'│', current_theme->border_active);
        surface.fill({w - 1, 1, 1, h - 2}, U'│', current_theme->border_active);
        surface.put(0, 0, "╭", current_theme->border_active);
        surface.put(w - 1, 0, "╮", current_theme->border_active);
        surface.put(0, h - 1, "╰", current_theme->border_active);
        surface.put(w - 1, h - 1, "╯", current_theme->border_active);
        surface.text(2, 0, (question ? "Choose — " : std::string(ui::text().approve_title)) + tool + " ",
                     current_theme->text);
        surface.text(std::max(2, (w - display_width(ui::text().approve_cancel)) / 2), h - 1, std::string(ui::text().approve_cancel), current_theme->text_muted);
    }
    void theme(const tui::ThemeTokens& theme) {
        current_theme = &theme;
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
    panel_->tool = approval.tool;
    panel_->theme(theme_);
    panel_->heading->set_text(" ");
    panel_->summary->set_text("  " + approval.reason);
    std::string keys = std::string(ui::text().approve_allow);
    if (!approval.session_rule.empty()) keys += std::string(ui::text().approve_session);
    if (approval.can_network) keys += std::string(ui::text().approve_network);
    keys += std::string(ui::text().approve_deny);
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
    case tools::Intent::Kind::ask:
    case tools::Intent::Kind::exit_plan: break;
    }
    std::string scope = approval.intent.summary;
    if (!approval.cwd.empty()) scope += "\ncwd: " + approval.cwd;
    if (!approval.mode.empty()) scope += "\nmode: " + approval.mode;
    for (const auto& request : approval.requests) {
        scope += "\n- " + request.reason;
        if (!request.target.empty() && request.target != approval.intent.command)
            scope += ": " + request.target;
    }
    if (approval.partially_executed) scope += "\nWarning: part of this call has already executed.";
    if (!approval.session_rule.empty()) scope += "\n[a] " + approval.session_rule;
    panel_->preview->document().append_block(tui::BlockKind::text, std::move(scope));
    panel_->preview->document().append_block(kind, preview);
    overlay_ = rt_.open_overlay(std::move(panel), tui::Placement::center, {}, this, panel_->feedback);
}

void ApprovalDialog::open(const agent::Question& question,
                          std::function<void(agent::Answer)> answer) {
    close();
    question_ = question;
    question_answer_ = std::move(answer);
    selected_.assign(question.options.size(), false);
    question_cursor_ = 0;
    auto panel = std::make_unique<Panel>(rt_);
    panel_ = panel.get();
    panel_->tool = question.header;
    panel_->question = true;
    panel_->theme(theme_);
    panel_->heading->set_text(" ");
    panel_->preview->document().append_block(tui::BlockKind::text, question.prompt);
    refresh_question();
    overlay_ = rt_.open_overlay(std::move(panel), tui::Placement::center, {}, this, panel_->feedback);
}

void ApprovalDialog::refresh_question() {
    if (!panel_ || !question_answer_) return;
    std::string choices;
    for (std::size_t i = 0; i < question_.options.size(); ++i) {
        const bool current = question_cursor_ == static_cast<int>(i);
        choices += current ? "▌" : " ";
        if (question_.multi_select) choices += selected_[i] ? "[x] " : "[ ] ";
        choices += std::format("{}  {}", i + 1, question_.options[i].label);
        if (!question_.options[i].description.empty())
            choices += " — " + question_.options[i].description;
        choices += '\n';
    }
    if (question_.allow_other) {
        choices += question_cursor_ == static_cast<int>(question_.options.size()) ? "▌" : " ";
        choices += std::format("{}  Other…", question_.options.size() + 1);
    }
    panel_->summary->set_text("  " + question_.prompt);
    panel_->choices->set_text(std::move(choices));
}

void ApprovalDialog::close() {
    if (!overlay_) return;
    rt_.unbind_mouse(*panel_->preview);
    rt_.close_overlay(overlay_); overlay_ = 0; panel_ = nullptr; answer_ = {}; question_answer_ = {};
}
void ApprovalDialog::set_theme(const tui::ThemeTokens& theme) {
    theme_ = theme; if (panel_) panel_->theme(theme_);
}
void ApprovalDialog::answer(agent::Decision decision) {
    auto callback = std::move(answer_); close(); if (callback) callback(std::move(decision));
}
void ApprovalDialog::answer(agent::Answer value) {
    auto callback = std::move(question_answer_); close(); if (callback) callback(std::move(value));
}
bool ApprovalDialog::on_event(const tui::Event& e) {
    if (!panel_) return false;
    if (tui::any(e.mods & tui::Mods::ctrl) && e.text == "c") { interrupt_(); return true; }
    if (e.key == tui::Key::page_up || e.key == tui::Key::page_down) {
        panel_->preview->scroll_pages(e.key == tui::Key::page_up ? -1 : 1); return true;
    }
    if (question_answer_) {
        const int count = static_cast<int>(question_.options.size()) + (question_.allow_other ? 1 : 0);
        if (panel_->feedback->visible) {
            if (e.key == tui::Key::escape) {
                panel_->feedback->visible = false; panel_->feedback->invalidate_layout();
            } else if (e.key == tui::Key::enter) {
                answer(agent::Answer{{}, panel_->feedback->text(), false});
            } else panel_->edit->on_event(e);
            return true;
        }
        if (e.key == tui::Key::escape) { answer(agent::Answer{{}, {}, true}); return true; }
        if (e.key == tui::Key::up || e.key == tui::Key::down) {
            question_cursor_ = (question_cursor_ + (e.key == tui::Key::up ? count - 1 : 1)) % count;
            refresh_question(); return true;
        }
        if (e.text.size() == 1 && e.text[0] >= '1' && e.text[0] <= '9') {
            const int index = e.text[0] - '1';
            if (index >= count) return true;
            question_cursor_ = index;
            if (index == static_cast<int>(question_.options.size())) {
                panel_->feedback->visible = true; panel_->feedback->invalidate_layout();
            } else if (question_.multi_select) {
                selected_[static_cast<std::size_t>(index)] = !selected_[static_cast<std::size_t>(index)];
                refresh_question();
            } else answer(agent::Answer{{index}, {}, false});
            return true;
        }
        if (e.text == " " && question_.multi_select &&
            question_cursor_ < static_cast<int>(question_.options.size())) {
            selected_[static_cast<std::size_t>(question_cursor_)] =
                !selected_[static_cast<std::size_t>(question_cursor_)];
            refresh_question(); return true;
        }
        if (e.key == tui::Key::enter) {
            if (question_cursor_ == static_cast<int>(question_.options.size())) {
                panel_->feedback->visible = true; panel_->feedback->invalidate_layout();
                return true;
            }
            std::vector<int> selected;
            if (question_.multi_select) {
                for (std::size_t i = 0; i < selected_.size(); ++i)
                    if (selected_[i]) selected.push_back(static_cast<int>(i));
                if (selected.empty()) return true;
            } else selected.push_back(question_cursor_);
            answer(agent::Answer{std::move(selected), {}, false});
        }
        return true;
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

agent::Answer ask(tui::Runtime& rt, ApprovalDialog& dialog,
                  const agent::Question& question, std::stop_token stop) {
    struct Pending { std::promise<agent::Answer> promise; std::atomic<bool> done{false}; };
    auto state = std::make_shared<Pending>();
    auto future = state->promise.get_future();
    auto answer = [state](agent::Answer value) {
        if (!state->done.exchange(true)) state->promise.set_value(std::move(value));
    };
    rt.post([&dialog, question, answer, state] {
        if (!state->done.load()) dialog.open(question, answer);
    });
    std::stop_callback cancelled(stop, [&] { answer(agent::Answer{{}, {}, true}); });
    auto result = future.get();
    if (stop.stop_requested()) rt.post([&dialog] { dialog.close(); });
    return result;
}
} // namespace dagent::ui
