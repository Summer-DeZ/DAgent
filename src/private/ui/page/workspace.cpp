#include "ui/page/workspace.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "ui/page/center.hpp"
#include "ui/style/appearance.hpp"
#include "ui/style/display.hpp"
#include "ui/style/strings.hpp"

namespace dagent::ui {

void QueueLabel::render(tui::Surface& s) {
    const auto& t = *theme_;
    s.fill({0, 0, s.cols(), s.rows()}, U' ', t.background);
    if (queue_.empty()) return;
    const auto hint = format_text(ui::text().queue_recall, icons().up);
    const int right = s.cols() - display_width(hint);
    int x = s.text(0, 0, ui::text().queue_label, t.text_muted) + 1;
    const int count = std::min(3, static_cast<int>(queue_.size()));
    const int gap = outline ? 2 : 1;
    int shown = 0;
    for (; shown < count; ++shown) {
        const int remaining = count - shown;
        const int more = queue_.size() > static_cast<size_t>(count) ? display_width(std::format("+{}", queue_.size() - count)) + 1 : 0;
        const int room = (right - x - more - 1) / remaining - gap - (outline ? 0 : 2);
        if (room < 1) break;
        const auto& value = queue_[shown].text_preview;
        const auto label = fit_columns(std::string_view(value).substr(0, value.find('\n')), std::min(24, room));
        x = draw_chip(s, x, 0, label, t.text, t, outline, t.background.bg) + gap;
    }
    if (queue_.size() > static_cast<size_t>(shown))
        s.text(x, 0, fit_columns(std::format("+{}", queue_.size() - shown), right - x - 1), t.text_muted);
    s.text(right, 0, hint, t.text_muted);
}

bool AgentTabs::on_event(const tui::Event& e) {
    if (e.kind != tui::Event::Kind::mouse || !e.mouse.press || e.mouse.motion || e.mouse.button != 0) return false;
    for (const auto& hit : hits_) if (e.mouse.x >= hit.begin && e.mouse.x < hit.end) { select(hit.index); return true; }
    return false;
}

void AgentTabs::render(tui::Surface& s) {
    const auto& t = *theme_; hits_.clear();
    s.fill({0, 0, s.cols(), s.rows()}, U' ', t.background);
    if (tabs.size() < 2) return;
    const auto hint = ui::text().agent_switch;
    const int right = std::max(0, s.cols() - display_width(hint) - 1);
    const int label_limit = std::max(4, (right - 10) / 2);
    const auto label = [&](size_t i) {
        return fit_columns(i == 0 ? tabs[i].name : std::string(ui::text().agent_task) + std::string(icons().separator) + tabs[i].name, label_limit);
    };
    size_t first = 1;
    int required = 0;
    for (size_t i = 1; i <= active; ++i) required += display_width(label(i)) + 4;
    while (first < active && required > right - 10) { required -= display_width(label(first)) + 4; ++first; }
    int x = 1;
    for (size_t i = 0; i < tabs.size(); i = i == 0 ? first : i + 1) {
        const auto name = label(i); const int width = display_width(name) + 2;
        if (x + width > right - 1) break;
        const auto state = tabs[i].state;
        const auto tone = state == AgentState::running ? t.accent : state == AgentState::failed ? t.error : t.success;
        s.text(x, 0, state == AgentState::running ? icons().spinner[frame] : state == AgentState::failed ? icons().cross : icons().check, tone);
        s.text(x + 2, 0, name, i == active ? t.text : t.text_muted);
        if (i == active) for (int col = x; col < x + width; ++col) s.put(col, 1, icons().horizontal, t.primary);
        hits_.push_back({x, x + width, i}); x += width + 2;
    }
    s.text(right, 0, hint, t.text_muted);
}

bool Page::dirty_tree() const noexcept {
    return tui::Container::dirty_tree() || (modal && modal() != dimmed_);
}

void Page::render(tui::Surface& surface) {
    invalidate_tree();
    tui::Container::render(surface);
    dimmed_ = modal && modal();
    if (dimmed_) dim(surface);
}

void Workspace::layout(tui::Rect area) {
    if (resize) resize(area.w);
    // Both panel widths are decided from the terminal width before laying out the main area.
    tui::Widget::layout(area);
    const int status = child(1).measure({area.w, area.h}).cols;
    const int navigator = child(2).measure({area.w, area.h}).cols;
    child(0).layout({left ? status : navigator, 0, std::max(0, area.w - status - navigator), area.h});
    child(1).layout({left ? 0 : area.w - status, 0, status, area.h});
    child(2).layout({left ? area.w - navigator : 0, 0, navigator, area.h});
}

tui::Scrollback& ScrollFrame::create() {
    auto scroll = std::make_unique<tui::Scrollback>();
    tui::Scrollback* pointer = scroll.get();
    scrolls_.push_back(std::move(scroll));
    if (current_ == nullptr) show(*pointer);
    return *pointer;
}

void ScrollFrame::show(tui::Scrollback& scroll) {
    if (current_ == &scroll) return;
    if (current_ != nullptr) disown(*current_);
    current_ = &scroll;
    adopt(*this, scroll);
    invalidate_tree();
}

void ScrollFrame::destroy(tui::Scrollback& scroll) {
    if (current_ == &scroll) {
        disown(scroll);
        current_ = nullptr;
    }
    std::erase_if(scrolls_, [&](const std::unique_ptr<tui::Scrollback>& item) {
        return item.get() == &scroll;
    });
}

void ScrollFrame::layout(tui::Rect area) {
    tui::Widget::layout(area);
    if (current_ != nullptr) current_->layout({0, 0, area.w, area.h});
    welcome.layout({0, 0, area.w, area.h});
}

void ScrollFrame::render(tui::Surface& surface) {
    if (welcome.visible) {
        if (current_) surface.fill({0, 0, surface.cols(), surface.rows()}, U' ', current_->theme().background);
        auto view = surface.view(welcome.rect());
        welcome.render(view); welcome.clear_dirty(); clear_dirty(); return;
    }
    if (current_ == nullptr) return;
    // 提示独占滚动区下方一行；先按完整视口判断，避免提示自身制造未读行。
    auto& doc = current_->document();
    doc.begin_frame(surface.cols(), current_->theme().epoch);
    const auto anchor = doc.anchor();
    const auto top = anchor.pinned_to_bottom ? doc.total_rows()
        : doc.row_of(anchor.block_id, anchor.byte_in_block, current_->theme()).value_or(0);
    const bool hint = !anchor.pinned_to_bottom && surface.rows() > 1 &&
        doc.total_rows() > top + static_cast<size_t>(surface.rows());
    const int height = surface.rows() - (hint ? 1 : 0);
    if (current_->rect().h != height) current_->layout({0, 0, surface.cols(), height});
    auto content = surface.view({0, 0, surface.cols(), height});
    current_->render(content); current_->clear_dirty();
    if (hint) {
        surface.fill({0, height, surface.cols(), 1}, U' ', current_->theme().background);
        const int width = reading_width(surface.cols());
        const auto text = fit_columns(format_text(ui::text().card_unseen, current_->unseen_rows()), width);
        const int x = (surface.cols() - width) / 2 + std::max(0, width - display_width(text));
        surface.text(x, height, text, current_->theme().text_faint);
    }
    clear_dirty();
}

bool ScrollFrame::dirty_tree() const noexcept {
    return dirty_ || (welcome.visible ? welcome.dirty_tree() : (current_ != nullptr && current_->dirty_tree()));
}

bool ScrollFrame::needs_layout() const noexcept {
    return layout_dirty_ || (current_ != nullptr && current_->needs_layout());
}

void ScrollFrame::invalidate_tree() noexcept {
    invalidate(); welcome.invalidate_tree();
    if (current_ != nullptr) current_->invalidate_tree();
}

void ScrollFrame::invalidate_rect(tui::Rect rect) {
    if (current_ != nullptr) current_->invalidate_rect(rect);
    Widget::invalidate_rect(rect);
}

tui::Widget* ScrollFrame::hit_test(tui::Point point) const noexcept {
    if (welcome.visible) return welcome.hit_test(point);
    if (current_ != nullptr) {
        if (auto* hit = current_->hit_test(point)) return hit;
    }
    return Widget::hit_test(point);
}

} // namespace dagent::ui
