#include "ui/appearance.hpp"
#include "ui/panel.hpp"
#include "ui/strings.hpp"
#include "ui/display.hpp"

#include <algorithm>
#include <cctype>
#include <utility>
#include "tui/grapheme.hpp"

namespace dagent::ui {

class Panel::View final : public tui::Widget {
public:
    Panel* owner = nullptr;
    std::string title, hint;
    const tui::ThemeTokens* theme = &tui::dark_theme();

    struct Line { int pos; std::string group; };
    std::vector<Line> lines() const {
        std::vector<Line> out;
        std::string previous;
        for (int pos = 0; pos < static_cast<int>(owner->shown_.size()); ++pos) {
            const auto& group = owner->rows_[owner->shown_[pos]].group;
            if (!group.empty() && group != previous) {
                if (!out.empty()) out.push_back({-1, {}});
                auto title = group;
                std::ranges::transform(title, title.begin(), [](unsigned char c) { return std::toupper(c); });
                out.push_back({-1, std::move(title)});
            }
            out.push_back({pos, {}}); previous = group;
        }
        return out;
    }
    tui::Size measure(tui::Size available) const override {
        const int rows = static_cast<int>(lines().size()) + 2 +
                         (owner->filter_ ? 2 + !owner->query_.empty() : 0);
        return {std::max(1, std::min(76, available.cols - 8)),
                std::max(3, std::min(rows, available.rows * 3 / 5))};
    }
    void render(tui::Surface& surface) override {
        const int w = surface.cols(), h = surface.rows();
        surface.fill({0, 0, w, h}, U' ', theme->background_panel);
        if (w < 4 || h < 3) return;
        auto panel = [&](tui::Style style) { style.bg = theme->background_panel.bg; return style; };
        draw_frame(surface, {0, 0, w, h}, panel(theme->border_active));
        surface.text(2, 0, fit_columns(" " + title + " ", w - 4), panel(theme->text));
        const std::string footer = fit_columns(" " + hint + " ", w - 4);
        surface.text((w - display_width(footer)) / 2, h - 1, footer, panel(theme->text_muted));
        int y = 1;
        if (owner->filter_) {
            surface.text(2, y++, fit_columns(std::string(ui::text().panel_search) + owner->query_, w - 4), panel(theme->text_muted));
            ++y;
        }
        const bool count = owner->filter_ && !owner->query_.empty();
        const int end = h - 1 - (count ? 1 : 0);
        if (count) surface.text(2, end, fit_columns(format_text(ui::text().panel_count,
            owner->shown_.size(), owner->rows_.size(), owner->noun_), w - 4), panel(theme->text_muted));
        const auto visual = lines();
        int selected_line = 0;
        for (int i = 0; i < static_cast<int>(visual.size()); ++i)
            if (visual[i].pos == owner->selected_) selected_line = i;
        const int first = std::max(0, selected_line - std::max(0, end - y) + 1);
        int alias_width = 0;
        for (const int index : owner->shown_) {
            alias_width = std::max(alias_width, display_width(owner->rows_[index].alias));
        }
        alias_width = std::min(alias_width, std::max(0, (w - 6) / 4));
        for (int i = first; i < static_cast<int>(visual.size()) && y < end; ++i, ++y) {
            const auto& line = visual[i];
            if (line.pos < 0) {
                surface.text(2, y, fit_columns(line.group, w - 4), panel(theme->text_muted)); continue;
            }
            const Row& row = owner->rows_[owner->shown_[line.pos]];
            const int right_width = std::min(display_width(row.right), std::max(0, (w - 6) / 3));
            const int right_col = w - 2 - right_width;
            const int alias_col = right_col - (right_width ? 2 : 0) - alias_width;
            const int content_end = alias_col - (alias_width ? 2 : 0);
            const bool selected = line.pos == owner->selected_ && row.enabled;
            if (selected) surface.fill({1, y, w - 2, 1}, U' ', theme->background_element);
            auto style = [&](tui::Style value) {
                value.bg = selected ? theme->background_element.bg : theme->background_panel.bg; return value;
            };
            surface.text(1, y, selected ? icons().bar : " ", style(theme->primary));
            const int room = std::max(0, content_end - 2);
            const int label_room = row.middle.empty() ? room : std::min(room, std::max(room / 2, std::min(display_width(row.label), room * 2 / 3)));
            const auto label = fit_columns(row.label, label_room);
            std::string folded = row.label, needle = owner->query_;
            const auto lower = [](unsigned char c) { return std::tolower(c); };
            std::ranges::transform(folded, folded.begin(), lower);
            std::ranges::transform(needle, needle.begin(), lower);
            const auto hit = needle.empty() ? std::string::npos : folded.find(needle);
            std::string_view rest = label;
            tui::unicode::Grapheme g;
            int x = 2; size_t offset = 0;
            while (tui::unicode::next_grapheme(rest, g)) {
                auto tone = row.enabled ? theme->text : theme->text_muted;
                if (hit != std::string::npos && offset < hit + needle.size() && offset + g.bytes.size() > hit) {
                    tone = theme->primary; tone.attrs = tone.attrs | tui::Attr::bold;
                }
                surface.put(x, y, g.bytes, style(tone)); x += g.width; offset += g.bytes.size();
            }
            if (!row.middle.empty()) surface.text(x + 2, y, fit_columns(row.middle, content_end - x - 2), style(theme->text_muted));
            surface.text(alias_col, y, fit_columns(row.alias, alias_width), style(theme->text_muted));
            const auto right = fit_columns(row.right, right_width);
            surface.text(w - 2 - display_width(right), y, right, style(theme->text_muted));
        }
    }
};

Panel::Panel(tui::Runtime& rt) : rt_(rt) {}
Panel::~Panel() { on_close_ = {}; close(); }
void Panel::open(std::string title, std::vector<Row> rows, std::string hint,
                 bool filter, std::function<void(int)> on_highlight,
                 std::function<void(bool)> on_close, int initial_row,
                 std::function<void()> on_add, std::string noun) {
    close(); rows_ = std::move(rows); filter_ = filter; on_highlight_ = std::move(on_highlight);
    on_close_ = std::move(on_close); on_add_ = std::move(on_add);
    query_.clear(); selected_ = 0; noun_ = noun.empty() ? std::string(ui::text().panel_items) : std::move(noun);
    auto view = std::make_unique<View>(); view_ = view.get(); view_->owner = this;
    view_->title = std::move(title); view_->hint = std::move(hint); view_->theme = &theme_;
    refilter();
    if (initial_row >= 0 && initial_row < static_cast<int>(shown_.size()) && rows_[initial_row].enabled)
        selected_ = static_cast<int>(std::ranges::find(shown_, initial_row) - shown_.begin());
    overlay_ = rt_.open_overlay(std::move(view), tui::Placement::center, {}, this);
}
void Panel::close(bool committed) {
    if (!overlay_) return;
    auto callback = std::move(on_close_);
    rt_.close_overlay(overlay_); overlay_ = 0; view_ = nullptr;
    rows_.clear(); shown_.clear(); query_.clear(); on_highlight_ = {}; on_close_ = {}; on_add_ = {};
    if (callback) callback(committed);
}
void Panel::set_theme(const tui::ThemeTokens& theme) {
    theme_ = theme; if (view_) { view_->theme = &theme_; view_->invalidate(); }
}
void Panel::refilter() {
    shown_.clear();
    std::string needle = query_;
    std::ranges::transform(needle, needle.begin(), [](unsigned char c) { return std::tolower(c); });
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        std::string label = rows_[static_cast<std::size_t>(i)].label;
        std::ranges::transform(label, label.begin(), [](unsigned char c) { return std::tolower(c); });
        if (needle.empty() || label.find(needle) != std::string::npos) shown_.push_back(i);
    }
    std::vector<std::string> groups;
    for (const int index : shown_)
        if (std::ranges::find(groups, rows_[index].group) == groups.end()) groups.push_back(rows_[index].group);
    std::stable_sort(shown_.begin(), shown_.end(), [&](int a, int b) {
        return std::ranges::find(groups, rows_[a].group) < std::ranges::find(groups, rows_[b].group);
    });
    selected_ = 0;
    while (selected_ < static_cast<int>(shown_.size()) &&
           !rows_[static_cast<std::size_t>(shown_[selected_])].enabled) ++selected_;
    if (selected_ >= static_cast<int>(shown_.size())) selected_ = 0;
    if (view_) view_->invalidate_layout();
}
void Panel::move(int delta) {
    if (shown_.empty()) return;
    int pos = selected_;
    for (int n = 0; n < static_cast<int>(shown_.size()); ++n) {
        pos = (pos + delta + static_cast<int>(shown_.size())) % static_cast<int>(shown_.size());
        if (rows_[static_cast<std::size_t>(shown_[pos])].enabled) break;
    }
    if (pos == selected_) return;
    selected_ = pos; view_->invalidate();
    if (on_highlight_) on_highlight_(shown_[selected_]);
}
bool Panel::on_event(const tui::Event& event) {
    if (!view_) return false;
    if (event.key == tui::Key::escape || (tui::any(event.mods & tui::Mods::ctrl) && event.text == "c")) {
        close(); return true;
    }
    if (event.key == tui::Key::up) { move(-1); return true; }
    if (event.key == tui::Key::down) { move(1); return true; }
    if (event.kind == tui::Event::Kind::text && event.text == "a" &&
        event.mods == tui::Mods::none && on_add_) {
        auto action = std::move(on_add_); close(true); action(); return true;
    }
    if (event.key == tui::Key::enter) {
        if (!shown_.empty() && rows_[shown_[selected_]].enabled) {
            auto action = rows_[static_cast<std::size_t>(shown_[selected_])].activate;
            close(true); if (action) action();
        }
        return true;
    }
    if (!filter_) return true;
    if (event.key == tui::Key::backspace) {
        if (!query_.empty()) {
            do { query_.pop_back(); } while (!query_.empty() &&
                (static_cast<unsigned char>(query_.back()) & 0xc0) == 0x80);
            refilter();
        }
        return true;
    }
    if (event.kind == tui::Event::Kind::text && !event.text.empty() && event.mods == tui::Mods::none) {
        query_ += event.text; refilter(); return true;
    }
    return true;
}

} // namespace dagent::ui
