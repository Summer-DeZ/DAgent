#include "ui/appearance.hpp"
#include "base/platform.hpp"
#include "ui/approval.hpp"
#include "ui/strings.hpp"
#include "ui/display.hpp"
#include "ui/theme_config.hpp"

#include <algorithm>
#include <format>
#include <cstdlib>
#include <set>
#include <utility>

namespace dagent::ui {
namespace {
class FeedbackInput final : public DialogInput {
public:
    bool visible = false;
    tui::Size measure(tui::Size available) const override {
        return visible ? tui::InputBox::measure(available) : tui::Size{};
    }
    std::optional<tui::Point> cursor() const override {
        return visible ? tui::InputBox::cursor() : std::nullopt;
    }
};

std::string request_kind(std::string_view kind) {
    const Strings& text = ui::text();
    if (kind == "dynamic_command") return std::string(text.approve_need_command);
    if (kind == "read_path") return std::string(text.approve_need_read);
    if (kind == "write_path") return std::string(text.approve_need_write);
    if (kind == "network") return std::string(text.approve_need_network);
    if (kind == "sensitive_read") return std::string(text.approve_need_sensitive);
    if (kind == "protected_write") return std::string(text.approve_need_protected);
    if (kind == "host_access") return std::string(text.approve_need_host);
    if (kind == "host_command") return std::string(text.approve_need_host_command);
    return std::string(kind);
}
std::string category(std::string_view kind) {
    const auto& t = ui::text();
    if (kind == "network") return std::string(t.approve_category_network);
    if (kind == "read_path") return std::string(t.approve_category_read);
    if (kind == "write_path") return std::string(t.approve_category_write);
    if (kind == "sensitive_read") return std::string(t.approve_category_sensitive);
    if (kind == "protected_write") return std::string(t.approve_category_protected);
    if (kind == "host_access") return std::string(t.approve_category_host);
    if (kind == "host_command") return std::string(t.approve_category_host_command);
    return std::string(t.approve_category_command);
}

class Options final : public tui::Widget {
public:
    std::vector<std::pair<std::string, std::string>> items;
    int selected = 0;
    const tui::ThemeTokens* theme = &tui::dark_theme();
    struct Part { int x, y, index; bool first; std::string label; };
    std::vector<Part> arrange(int width) const {
        std::vector<Part> out;
        int x = 0, y = 0;
        const int room = std::max(1, width - 4);
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const auto value = items[i].first + " " + items[i].second;
            if (x && x + display_width(value) + 4 > width) { x = 0; ++y; }
            for (size_t at = 0; at < value.size();) {
                const auto edge = tui::wrap_next_row(value, at, room);
                std::string label; tui::expand_row(label, value, at, edge.end);
                out.push_back({x, y, i, at == 0, label});
                at = edge.next;
                if (at < value.size()) { x = 0; ++y; }
                else x += display_width(label) + 5;
            }
        }
        return out;
    }
    tui::Size measure(tui::Size available) const override {
        const auto parts = arrange(available.cols);
        return {available.cols, parts.empty() ? 0 : parts.back().y + 1};
    }
    void render(tui::Surface& s) override {
        for (const auto& part : arrange(s.cols())) {
            const auto& t = *theme;
            auto tone = t.primary; tone.bg = t.background_element.bg; tone.attrs = tone.attrs | tui::Attr::bold;
            draw_chip(s, part.x, part.y, "  " + part.label, t.text, t, false, t.background_panel.bg);
            s.text(part.x + 1, part.y, part.index == selected ? icons().bar : " ", tone);
            if (part.first) s.text(part.x + 3, part.y, items[part.index].first, tone);
        }
    }
};

class PreviewRenderer final : public tui::BlockRenderer {
public:
    PreviewRenderer(std::unique_ptr<tui::BlockRenderer> inner, const tui::Color& background, bool command = false)
        : inner_(std::move(inner)), background_(background), command_(command) {}
    tui::WrapResult measure(std::string_view source, size_t from, int width) const override {
        return inner_->measure(source, from, width);
    }
    size_t render(const tui::Block& block, int width, const tui::ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<tui::Line>& out) const override {
        auto copy = block;
        if (command_) copy.meta = "bash";
        const auto count = inner_->render(copy, width, theme, from, valid, out);
        for (size_t i = valid; i < count; ++i) {
            auto& line = out[i];
            if (command_ && !line.spans.empty()) {
                line.spans.front().text = i == 0 ? "$ " : "  ";
                line.spans.front().style = theme.text_muted;
            }
            for (auto& span : line.spans) span.style.bg = background_;
            auto pad = theme.text; pad.bg = background_;
            if (line.width < width) line.spans.push_back({std::string(width - line.width, ' '), pad, tui::k_no_src});
            line.width = width;
        }
        return count;
    }
private:
    std::unique_ptr<tui::BlockRenderer> inner_;
    const tui::Color& background_;
    bool command_;
};

class DetailRenderer final : public tui::BlockRenderer {
public:
    tui::WrapResult measure(std::string_view source, size_t from, int width) const override {
        return tui::wrap_measure_from(source, from, std::max(1, width - 8));
    }
    size_t render(const tui::Block& block, int width, const tui::ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<tui::Line>& out) const override {
        const auto count = tui::TextRenderer().render(block, std::max(1, width - 8), theme, from, valid, out);
        for (size_t i = valid; i < count; ++i) {
            auto label = i == 0 ? fit_columns(block.meta, 7) : "";
            label.append(std::max(0, 8 - display_width(label)), ' ');
            out[i].spans.insert(out[i].spans.begin(), {label, theme.text_muted, tui::k_no_src});
            out[i].width += 8;
        }
        return count;
    }
};
} // namespace

class ApprovalDialog::Panel final : public tui::Container {
public:
    tui::Scrollback* preview;
    tui::Text *heading, *summary, *choices;
    Options* options;
    tui::ThemeTokens panel_theme_;
    tui::Color page_background;
    FeedbackInput* feedback;
    std::unique_ptr<tui::InputBoxHandler> edit;
    std::unique_ptr<tui::ScrollbackMouse> mouse;
    const tui::ThemeTokens* current_theme = &tui::dark_theme();
    std::string tool;
    std::string category_label, hint;
    bool dangerous = false;
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
        auto keys = std::make_unique<Options>(); options = keys.get();
        add({tui::Sizing::content}, std::move(keys));
        auto input = std::make_unique<FeedbackInput>(); feedback = input.get();
        add({tui::Sizing::content, 0, 0, 6}, std::move(input));
        // 为底边提示保留一行，不能覆盖审批选项或反馈输入。
        add({tui::Sizing::fixed, 1}, std::make_unique<tui::Text>());
        edit = std::make_unique<tui::InputBoxHandler>(*feedback);
        mouse = std::make_unique<tui::ScrollbackMouse>(rt, *preview);
        rt.bind_mouse(*preview, *mouse);
    }
    tui::Size measure(tui::Size available) const override {
        const int cols = std::min(100, std::max(1, available.cols - 4));
        const int width = std::max(0, cols - 4);
        const int summary_rows = summary->text().empty() ? 0 : summary->measure({width, available.rows}).rows;
        const int choice_rows = question ? choices->measure({width, available.rows}).rows : options->measure({width, available.rows}).rows;
        const int feedback_rows = feedback->visible ? std::min(6, feedback->measure({width, available.rows}).rows) : 0;
        auto& doc = preview->document();
        doc.begin_frame(width, current_theme->epoch);
        const int rows = 4 + summary_rows + (summary_rows ? 1 : 0) +
                         static_cast<int>(doc.total_rows()) + choice_rows + feedback_rows;
        return {cols, std::min(rows, std::min(available.rows, std::max(8, available.rows * 3 / 5)))};
    }
    void layout(tui::Rect area) override {
        tui::Widget::layout(area);
        const int width = std::max(0, area.w - 4);
        heading->layout({2, 0, width, 0});
        const int summary_rows = summary->text().empty() ? 0 : summary->measure({width, area.h}).rows;
        const int choice_rows = question ? choices->measure({width, area.h}).rows : options->measure({width, area.h}).rows;
        const int feedback_rows = feedback->visible ? std::min(6, feedback->measure({width, area.h}).rows) : 0;
        const int preview_y = 1 + summary_rows + (summary_rows ? 1 : 0);
        const int preview_rows = std::max(0, area.h - preview_y - choice_rows - feedback_rows - 3);
        summary->layout({2, 1, width, summary_rows});
        preview->layout({2, preview_y, width, preview_rows});
        const int choices_y = preview_y + preview_rows + 1;
        choices->layout({2, choices_y, width, question ? choice_rows : 0});
        options->layout({2, choices_y, width, question ? 0 : choice_rows});
        feedback->layout({2, choices_y + choice_rows, width, feedback_rows});
    }
    void render(tui::Surface& surface) override {
        surface.fill({0, 0, surface.cols(), surface.rows()}, U' ', current_theme->background_panel);
        tui::Container::render(surface);
        const int w = surface.cols(), h = surface.rows();
        if (w < 4 || h < 3) return;
        auto border = question ? current_theme->primary : dangerous ? current_theme->error : current_theme->warning;
        border.bg = current_theme->background_panel.bg;
        draw_frame(surface, {0, 0, w, h}, border);
        auto title_style = current_theme->text; title_style.attrs = title_style.attrs | tui::Attr::bold;
        const auto title = question ? tool : std::string(ui::text().approve_title);
        const int category_width = question ? 0 : chip_width(category_label, false) + 1;
        const int x = surface.text(2, 0, " " + fit_columns(title, w - 6 - category_width) + " ", title_style);
        if (!question) draw_chip(surface, x + 1, 0, fit_columns(category_label, w - x - 5), border,
                                 *current_theme, false, current_theme->background_panel.bg);
        draw_hint(surface, 2, h - 2, fit_columns(hint, w - 4), current_theme->text_muted, current_theme->text_faint);
    }
    void theme(const tui::ThemeTokens& theme) {
        page_background = theme.background.bg;
        panel_theme_ = panel_theme(theme); current_theme = &panel_theme_;
        heading->set_theme(panel_theme_); heading->set_style(panel_theme_.warning);
        summary->set_theme(panel_theme_); summary->set_style(panel_theme_.text);
        choices->set_theme(panel_theme_); choices->set_style(panel_theme_.text);
        options->theme = &panel_theme_;
        preview->set_theme(panel_theme_); feedback->set_theme(panel_theme_);
    }
};

ApprovalDialog::ApprovalDialog(tui::Runtime& rt, std::function<void()> interrupt)
    : rt_(rt), interrupt_(std::move(interrupt)) {}
ApprovalDialog::~ApprovalDialog() { close(); }

void ApprovalDialog::open(const ApprovalRequest& approval,
                           std::function<void(ApprovalAnswer)> answer) {
    close(); approval_ = approval; answer_ = std::move(answer);
    allow_cycle_ = false;
    auto panel = std::make_unique<Panel>(rt_); panel_ = panel.get();
    panel_->tool = approval.tool;
    std::set<std::string> kinds;
    panel_->dangerous = approval.partially_executed;
    for (const auto& request : approval.requests) {
        kinds.insert(request.kind);
        if (request.kind == "host_access" || request.kind == "protected_write" || request.kind == "sensitive_read") panel_->dangerous = true;
    }
    panel_->category_label = category(approval.requests.empty() ? "dynamic_command" : approval.requests.front().kind);
    if (kinds.size() > 1) panel_->category_label += std::format(" +{}", kinds.size() - 1);
    panel_->theme(theme_);
    panel_->summary->set_text(approval.summary);
    panel_->options->items.push_back({"y", std::string(ui::text().approve_once)});
    if (!approval.session_rule.empty()) panel_->options->items.push_back({"a", std::string(ui::text().approve_session_option)});
    panel_->options->items.push_back({"n", std::string(ui::text().approve_no)});
    panel_->options->items.push_back({"e", std::string(ui::text().approve_explain)});
    panel_->hint = format_text(ui::text().approve_hint, approval.session_rule.empty() ? "y n e" : "y a n e",
                              icons().bar == "|" ? "<" : "←", icons().bar == "|" ? ">" : "→");
    auto& doc = panel_->preview->document();
    doc.set_renderer(tui::BlockKind::text, std::make_unique<DetailRenderer>());
    const bool command = approval.tool == "bash" || kinds.contains("dynamic_command");
    auto kind = command || approval.preview_kind == "code" ? tui::BlockKind::code
              : approval.preview_kind == "diff" ? tui::BlockKind::diff : tui::BlockKind::output;
    if (kind == tui::BlockKind::code)
        doc.set_renderer(kind, std::make_unique<PreviewRenderer>(std::make_unique<tui::SyntaxRenderer>(), panel_->page_background, command));
    else if (kind == tui::BlockKind::diff)
        doc.set_renderer(kind, std::make_unique<PreviewRenderer>(std::make_unique<tui::DiffRenderer>(), panel_->page_background));
    else doc.set_renderer(kind, std::make_unique<PreviewRenderer>(std::make_unique<tui::TextRenderer>(), panel_->page_background));
    if (!approval.preview_text.empty()) doc.append_block(kind, approval.preview_text);
    auto detail = [&](std::string_view label, std::string value) {
        if (value.empty()) return;
        const auto id = doc.append_block(tui::BlockKind::text, std::move(value));
        doc.set_meta(id, std::string(label));
    };
    auto cwd = approval.cwd;
    if (const auto home = base::home_directory()) {
        const std::string& prefix = *home;
        if (cwd == prefix || cwd.starts_with(prefix + "/")) cwd = "~" + cwd.substr(prefix.size());
    }
    detail(ui::text().approve_cwd, cwd);
    detail(ui::text().approve_mode, approval.mode);
    for (const auto& request : approval.requests) {
        auto value = request_kind(request.kind);
        if (!request.target.empty()) value += " " + request.target;
        if (!request.reason.empty()) value += " (" + request.reason + ")";
        detail(ui::text().approve_needs, std::move(value));
    }
    detail(ui::text().approve_reason, approval.reason);
    if (!approval.session_rule.empty()) detail(ui::text().approve_session_label, approval.session_rule);
    if (!approval.agent.empty()) detail(ui::text().approve_via, format_text(ui::text().approve_task, approval.agent));
    if (kinds.contains("host_access")) detail({}, std::string(ui::text().approve_host_boundary));
    if (kinds.contains("host_command")) detail({}, std::string(ui::text().approve_host_only_boundary));
    if (approval.partially_executed) detail({}, std::string(ui::text().approve_partial));
    if (approval.session_rule.empty()) detail({}, std::string(ui::text().approve_once_only));
    panel_->preview->scroll_home();
    overlay_ = rt_.open_overlay(std::move(panel), tui::Placement::center, {}, this, panel_->feedback);
}

void ApprovalDialog::open(const QuestionRequest& question,
                          std::function<void(QuestionAnswer)> answer) {
    close();
    question_ = question;
    question_answer_ = std::move(answer);
    allow_cycle_ = question.allow_permission_cycle;
    selected_.assign(question.options.size(), false);
    const int count = static_cast<int>(question.options.size()) + (question.allow_other ? 1 : 0);
    question_cursor_ = count > 0 ? std::clamp(question.default_option, 0, count - 1) : 0;
    auto panel = std::make_unique<Panel>(rt_);
    panel_ = panel.get();
    panel_->tool = question.header;
    panel_->question = true;
    panel_->hint = format_text(ui::text().question_hint, icons().up, icons().down);
    panel_->theme(theme_);
    panel_->heading->set_text(" ");
    panel_->preview->document().append_block(tui::BlockKind::text, question.prompt);
    refresh_question();
    panel_->preview->scroll_home();
    overlay_ = rt_.open_overlay(std::move(panel), tui::Placement::center, {}, this, panel_->feedback);
}

void ApprovalDialog::refresh_question() {
    if (!panel_ || !question_answer_) return;
    std::string choices;
    for (std::size_t i = 0; i < question_.options.size(); ++i) {
        const bool current = question_cursor_ == static_cast<int>(i);
        choices += current ? icons().bar : " ";
        if (question_.multi_select) choices += selected_[i] ? "[x] " : "[ ] ";
        choices += std::format("{}  {}", i + 1, question_.options[i].label);
        if (!question_.options[i].description.empty())
            choices += " — " + question_.options[i].description;
        choices += '\n';
    }
    if (question_.allow_other) {
        choices += question_cursor_ == static_cast<int>(question_.options.size()) ? icons().bar : " ";
        choices += std::format("{}  Other…", question_.options.size() + 1);
    }
    if (!choices.empty() && choices.back() == '\n') choices.pop_back();
    panel_->summary->set_text("");
    panel_->choices->set_text(std::move(choices));
}

void ApprovalDialog::close() {
    if (!overlay_) return;
    rt_.unbind_mouse(*panel_->preview);
    rt_.close_overlay(overlay_); overlay_ = 0; panel_ = nullptr; answer_ = {}; question_answer_ = {};
}
void ApprovalDialog::set_theme(const tui::ThemeTokens& theme) {
    theme_ = theme;
    if (panel_) {
        panel_->theme(theme_);
        if (question_answer_) refresh_question();
        panel_->invalidate_tree();
    }
}
void ApprovalDialog::answer(ApprovalAnswer decision) {
    auto callback = std::move(answer_); close(); if (callback) callback(std::move(decision));
}
void ApprovalDialog::answer(QuestionAnswer value) {
    auto callback = std::move(question_answer_); close(); if (callback) callback(std::move(value));
}
bool ApprovalDialog::on_event(const tui::Event& e) {
    if (!panel_) return false;
    if (tui::any(e.mods & tui::Mods::ctrl) && e.text == "c") { interrupt_(); return true; }
    if (e.key == tui::Key::page_up || e.key == tui::Key::page_down) {
        panel_->preview->scroll_pages(e.key == tui::Key::page_up ? -1 : 1); return true;
    }
    if (question_answer_) {
        // unrestricted 确认框：Shift+Tab 交给全局键位（取消并跳到 ask）。
        if (allow_cycle_ && e.kind == tui::Event::Kind::key && e.key == tui::Key::tab &&
            tui::any(e.mods & tui::Mods::shift))
            return false;
        const int count = static_cast<int>(question_.options.size()) + (question_.allow_other ? 1 : 0);
        if (panel_->feedback->visible) {
            if (e.key == tui::Key::escape) {
                panel_->feedback->visible = false; panel_->feedback->invalidate_layout();
            } else if (e.key == tui::Key::enter) {
                answer(QuestionAnswer{{}, panel_->feedback->text(), false});
            } else panel_->edit->on_event(e);
            return true;
        }
        if (e.key == tui::Key::escape) { answer(QuestionAnswer{{}, {}, true}); return true; }
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
            } else answer(QuestionAnswer{{index}, {}, false});
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
            answer(QuestionAnswer{std::move(selected), {}, false});
        }
        return true;
    }
    if (panel_->feedback->visible) {
        if (e.key == tui::Key::escape) {
            panel_->feedback->visible = false; panel_->feedback->invalidate_layout();
        } else if (e.key == tui::Key::enter) {
            answer({ApprovalAnswer::Decision::deny_with_feedback, panel_->feedback->text()});
        } else panel_->edit->on_event(e);
        return true;
    }
    if (e.key == tui::Key::left || e.key == tui::Key::right) {
        const int count = static_cast<int>(panel_->options->items.size());
        auto& selected = panel_->options->selected;
        selected = (selected + (e.key == tui::Key::left ? count - 1 : 1)) % count;
        panel_->options->invalidate(); return true;
    }
    const auto choice = e.key == tui::Key::enter ? panel_->options->items[panel_->options->selected].first : e.text;
    if (e.key == tui::Key::escape || choice == "n") answer({ApprovalAnswer::Decision::deny, {}});
    else if (choice == "y") answer({ApprovalAnswer::Decision::allow, {}});
    else if (choice == "a" && !approval_.session_rule.empty()) answer({ApprovalAnswer::Decision::allow_session, {}});
    else if (choice == "e") {
        panel_->feedback->visible = true; panel_->feedback->invalidate_layout();
    }
    return true;
}
} // namespace dagent::ui
