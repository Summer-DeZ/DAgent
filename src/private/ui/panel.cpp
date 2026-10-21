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
    std::vector<std::string> footer_lines(int width) const {
        std::vector<std::string> out;
        if (width < 1) return out;
        std::string_view remaining = owner->presentation_.footer;
        while (!remaining.empty()) {
            auto rest = remaining;
            tui::unicode::Grapheme g;
            int cols = 0;
            std::size_t bytes = 0, space = 0;
            while (tui::unicode::next_grapheme(rest, g) && cols + g.width <= width) {
                if (g.bytes == " ") space = bytes;
                cols += g.width; bytes += g.bytes.size();
            }
            if (!bytes) break;
            if (bytes < remaining.size() && space) bytes = space;
            out.emplace_back(remaining.substr(0, bytes));
            remaining.remove_prefix(bytes);
            while (remaining.starts_with(' ')) remaining.remove_prefix(1);
        }
        return out;
    }
    std::vector<Line> lines() const {
        std::vector<Line> out;
        std::string previous;
        for (int pos = 0; pos < static_cast<int>(owner->shown_.size()); ++pos) {
            const auto& group = owner->rows_[owner->shown_[pos]].group;
            if (!group.empty() && group != previous) {
                if (!out.empty()) out.push_back({-1, {}});
                auto heading = group;
                std::ranges::transform(heading, heading.begin(), [](unsigned char c) { return std::toupper(c); });
                out.push_back({-1, std::move(heading)});
            }
            out.push_back({pos, {}}); previous = group;
        }
        return out;
    }
    tui::Size measure(tui::Size available) const override {
        details_ = available.cols >= 80 && bool(owner->presentation_.details);
        const int width = std::max(1, std::min(owner->presentation_.width,
            available.cols - (owner->presentation_.width == 76 ? 8 : 4)));
        const int rows = static_cast<int>(lines().size() + footer_lines(width - 4).size()) + 2 +
                         (owner->filter_ ? 2 + !owner->query_.empty() : 0) +
                         !owner->presentation_.status.empty();
        const int height = owner->presentation_.height;
        return {width,
                std::max(1, height ? std::min(height, available.rows - 2)
                                  : std::min(rows, available.rows * 3 / 5))};
    }
    mutable bool details_ = false;
    void render(tui::Surface& surface) override {
        const int full_width = surface.cols(), h = surface.rows();
        const int w = details_ ? full_width / 2 : full_width;
        surface.fill({0, 0, full_width, h}, U' ', theme->background_panel);
        if (full_width < 4 || h < 3) return;
        auto panel = [&](tui::Style style) { style.bg = theme->background_panel.bg; return style; };
        draw_frame(surface, {0, 0, full_width, h}, panel(theme->border_active));
        surface.text(2, 0, fit_columns(" " + title + " ", full_width - 4), panel(theme->text));
        const std::string footer = fit_columns(" " + hint + " ", full_width - 4);
        draw_hint(surface, (full_width - display_width(footer)) / 2, h - 1, footer, panel(theme->text_muted), theme->text_faint);
        if (details_) {
            for (int y = 1; y < h - 1; ++y) surface.text(w, y, icons().vertical, panel(theme->border));
            auto detail = surface.view({w + 2, 2, full_width - w - 4, h - 4});
            owner->presentation_.details(detail, owner->shown_.empty() ? -1 : owner->shown_[owner->selected_]);
        }
        int y = 1;
        if (owner->filter_) {
            surface.text(2, y++, fit_columns(std::string(ui::text().panel_search) + owner->query_, w - 4), panel(theme->text_muted));
        }
        if (!owner->presentation_.status.empty())
            surface.text(2, y++, fit_columns(owner->presentation_.status, w - 4), panel(theme->text_muted));
        if (owner->filter_) ++y;
        const bool count = owner->filter_ && !owner->query_.empty();
        const auto source = footer_lines(w - 4);
        const int source_rows = std::min(static_cast<int>(source.size()), std::max(0, h - y - 3));
        const int end = h - 1 - (count ? 1 : 0) - source_rows;
        for (int i = 0; i < source_rows; ++i)
            surface.text(2, h - 1 - source_rows + i, source[i], panel(theme->text_muted));
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
        const bool aligned = owner->presentation_.align_columns;
        int middle_width = 0, tag_width = 0;
        if (aligned) for (const auto& row : owner->rows_) {
            middle_width = std::max(middle_width, display_width(row.middle));
            tag_width = std::max(tag_width, display_width(row.right));
        }
        for (int i = first; i < static_cast<int>(visual.size()) && y < end; ++i, ++y) {
            const auto& line = visual[i];
            if (line.pos < 0) {
                surface.text(2, y, fit_columns(line.group, w - 4), panel(theme->text_muted)); continue;
            }
            const Row& row = owner->rows_[owner->shown_[line.pos]];
            const int right_width = std::min(aligned ? tag_width : display_width(row.right), std::max(0, (w - 6) / 2));
            const int right_col = w - 2 - right_width;
            const int alias_col = right_col - (right_width ? 2 : 0) - alias_width;
            const int content_end = alias_col - (alias_width ? 2 : 0);
            const bool selected = line.pos == owner->selected_ && row.enabled;
            if (selected) surface.fill({1, y, w - 2, 1}, U' ', theme->background_selected);
            auto style = [&](tui::Style value) {
                value.bg = selected ? theme->background_selected.bg : theme->background_panel.bg; return value;
            };
            auto tone_style = [&](Tone tone) {
                switch (tone) {
                case Tone::muted: return theme->text_muted;
                case Tone::warning: return theme->warning;
                case Tone::error: return theme->error;
                default: return theme->text;
                }
            };
            surface.text(1, y, selected ? icons().bar : " ", style(theme->primary));
            const int room = std::max(0, content_end - 2);
            const int middle_room = std::min(middle_width, room / 2);
            const int label_room = aligned ? std::max(0, room - middle_room - 2) :
                row.middle.empty() ? room : std::min(room, std::max(room / 2, std::min(display_width(row.label), room * 2 / 3)));
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
                auto tone = tone_style(!row.enabled && row.tone == Tone::normal ? Tone::muted : row.tone);
                if (row.enabled && hit != std::string::npos && offset < hit + needle.size() && offset + g.bytes.size() > hit) {
                    tone = theme->primary; tone.attrs = tone.attrs | tui::Attr::bold;
                }
                surface.put(x, y, g.bytes, style(tone)); x += g.width; offset += g.bytes.size();
            }
            const int middle_col = aligned ? content_end - middle_room : x + 2;
            if (!row.middle.empty()) surface.text(middle_col, y, fit_columns(row.middle, content_end - middle_col), style(theme->text_muted));
            surface.text(alias_col, y, fit_columns(row.alias, alias_width), style(theme->text_muted));
            const auto right = fit_columns(row.right, right_width);
            surface.text(w - 2 - display_width(right), y, right, style(tone_style(row.right_tone)));
        }
    }
};

Panel::Panel(tui::Runtime& rt) : rt_(rt) {}
Panel::~Panel() { on_close_ = {}; close(); }
void Panel::open(std::string title, std::vector<Row> rows, std::string hint,
                 bool filter, std::function<void(int)> on_highlight,
                 std::function<void(bool)> on_close, int initial_row,
                 std::function<void()> on_add, std::string noun) {
    close(); presentation_ = {}; rows_ = std::move(rows); filter_ = filter; on_highlight_ = std::move(on_highlight);
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
void Panel::present(Presentation value) {
    presentation_ = std::move(value);
    if (view_) view_->invalidate_layout();
}
void Panel::update_rows(std::vector<Row> rows, bool reset_filter) {
    const int index = shown_.empty() ? 0 : shown_[selected_];
    rows_ = std::move(rows);
    if (reset_filter) query_.clear();
    refilter();
    const auto found = std::ranges::find(shown_, index);
    if (found != shown_.end() && rows_[index].enabled)
        selected_ = static_cast<int>(found - shown_.begin());
}
void Panel::refilter() {
    shown_.clear();
    std::string needle = query_;
    std::ranges::transform(needle, needle.begin(), [](unsigned char c) { return std::tolower(c); });
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const auto& row = rows_[static_cast<std::size_t>(i)];
        std::string label = row.label + "\n" + row.search_text;
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
    if (event.key == tui::Key::right && presentation_.open_right) {
        auto action = presentation_.open_right;
        if (!shown_.empty()) action(shown_[selected_]);
        return true;
    }
    if (event.kind == tui::Event::Kind::text && event.text == "r" &&
        event.mods == tui::Mods::none && presentation_.refresh) {
        auto action = presentation_.refresh; action(); return true;
    }
    if (event.kind == tui::Event::Kind::text && event.text == "a" &&
        event.mods == tui::Mods::none && on_add_) {
        auto action = std::move(on_add_); close(true); action(); return true;
    }
    if (event.key == tui::Key::enter) {
        if (!shown_.empty() && rows_[shown_[selected_]].enabled) {
            auto action = rows_[static_cast<std::size_t>(shown_[selected_])].activate;
            if (presentation_.close_on_activate) close(true);
            if (action) action();
        }
        return true;
    }
    if (!filter_) return true;
    if (event.key == tui::Key::backspace) {
        if (!query_.empty()) {
            auto start = query_.size() - 1;
            while (start && (static_cast<unsigned char>(query_[start]) & 0xc0) == 0x80) --start;
            query_.resize(start);
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
