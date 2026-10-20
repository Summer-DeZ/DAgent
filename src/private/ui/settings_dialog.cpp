#include "ui/settings_dialog.hpp"
#include "ui/display.hpp"
#include "ui/strings.hpp"
#include <algorithm>
#include <charconv>

namespace dagent::ui {
class SettingsDialog::View final : public tui::Widget {
public:
    explicit View(SettingsDialog& owner) : d(owner) {}
    SettingsDialog& d;
    mutable bool preview = true;
    tui::Size measure(tui::Size a) const override {
        preview = a.cols >= 80;
        return {std::min(100, std::max(1, a.cols - 4)), std::min(24, std::max(1, a.rows - 2))};
    }
    void render(tui::Surface& s) override {
        const auto& t = d.theme_;
        const auto style = [&](tui::Style v) { v.bg = t.background_panel.bg; return v; };
        const int w = s.cols(), h = s.rows();
        s.fill({0, 0, w, h}, U' ', t.background_panel);
        draw_frame(s, {0, 0, w, h}, style(t.border_active));
        s.text(2, 0, fit_columns(" " + std::string(text().settings_title) + " ", w - 4), style(t.text));
        const int nav = w >= 60 ? 16 : 2;
        if (nav > 2) {
            s.text(2, 2, text().settings_section, style(d.layout_page_ ? t.text_muted : t.primary));
            s.text(2, 3, text().settings_layout, style(d.layout_page_ ? t.primary : t.text_muted));
        }
        if (d.layout_page_) {
            const int width = w - nav - 2;
            const std::string_view labels[]{text().settings_width, text().settings_navigator, text().settings_status};
            const std::string values[]{d.width_custom_ ? "custom " + (d.editing_ ? d.hex_ + "_" : d.layout_.width.dump()) : d.layout_.width.get<std::string>(), d.layout_.navigator, d.layout_.status_panel};
            for (int i = 0; i < 3 && i + 2 < h - 3; ++i) {
                auto tone = style(t.text), muted = style(t.text_muted);
                if (d.row_ == i) {
                    s.fill({nav, i + 2, width, 1}, U' ', t.background_selected);
                    tone.bg = muted.bg = t.background_selected.bg;
                    auto bar = t.primary; bar.bg = tone.bg; s.text(nav, i + 2, icons().bar, bar);
                }
                s.text(nav + 2, i + 2, fit_columns(labels[i], 13), tone);
                if (d.invalid_ && i == 0) { muted.fg = t.error.fg; }
                s.text(nav + 16, i + 2, fit_columns(values[i], width - 17), muted);
            }
            const int col_width = std::max(1, width / 3), room = std::max(1, h - 11);
            const int selected = d.row_ - 3;
            const int first = std::max(0, selected - room + 1);
            const std::string_view titles[]{text().settings_column_navigator, text().settings_column_status, text().settings_column_hidden};
            for (int col = 0; col < 3; ++col) {
                const int x = nav + col * col_width;
                s.text(x + 1, 6, fit_columns(titles[col], col_width - 1), style(t.text_muted));
                const auto& ids = d.column(col);
                for (int i = first; i < static_cast<int>(ids.size()) && i - first < room; ++i) {
                    const int y = 7 + i - first; auto tone = style(t.text);
                    if (col == d.column_ && selected == i) {
                        s.fill({x, y, col_width, 1}, U' ', t.background_selected);
                        tone.bg = t.background_selected.bg; auto bar = t.primary; bar.bg = tone.bg;
                        s.text(x, y, icons().bar, bar);
                    }
                    s.text(x + 1, y, fit_columns(section_name(ids[i]), col_width - 2), tone);
                }
            }
            if (h > 4) {
                s.text(2, h - 3, fit_columns(d.invalid_ ? text().settings_width_invalid : text().settings_layout_footer, w - 4), style(d.invalid_ ? t.error : t.text_muted));
                draw_hint(s, 2, h - 2, fit_columns(d.saving_ ? text().settings_saving : text().settings_layout_keys, w - 4), style(t.text_muted), t.text_faint);
            }
            return;
        }
        const int end = preview ? w - 29 : w - 2;
        const int row_width = std::max(1, end - nav);
        const std::string_view names[] = {text().settings_mode, text().settings_background,
            text().settings_accent, text().settings_panels, text().settings_density, text().settings_icons, text().settings_diff, text().settings_links};
        const int room = std::max(1, h - 6), first = std::max(0, d.row_ - room + 1);
        for (int i = first; i < 8 && i - first < room; ++i) {
            const int y = 2 + i - first;
            auto normal = style(t.text), muted = style(t.text_muted);
            if (i == d.row_) {
                s.fill({nav, y, row_width, 1}, U' ', t.background_selected);
                normal.bg = muted.bg = t.background_selected.bg;
                auto bar = t.primary; bar.bg = normal.bg; s.text(nav, y, icons().bar, bar);
            }
            const int label_width = std::min(12, row_width / 2);
            s.text(nav + 2, y, fit_columns(names[i], label_width - 1), normal);
            auto tone = d.invalid_ && i == d.row_ ? style(t.error) : muted;
            tone.bg = normal.bg;
            const std::string value = d.editing_ && i == d.row_ ? d.hex_ + "_" : d.value(i);
            s.text(nav + 2 + label_width, y, fit_columns(value, row_width - label_width - 3), tone);
        }
        if (preview) {
            const int x = end + 2, width = w - x - 2;
            s.text(x, 2, fit_columns(text().settings_preview, width), style(t.text_muted));
            auto sample = [&](int y, std::string_view label, tui::Style tone, tui::Color bg) {
                if (y >= h - 3) return;
                tone.bg = bg; s.fill({x, y, width, 1}, U' ', tone);
                s.text(x + 1, y, fit_columns(label, width - 2), tone);
            };
            sample(4, std::string(icons().brand) + " " + std::string(text().brand), t.primary,
                   d.current_.panels == "outline" ? t.background.bg : t.background_panel.bg);
            sample(6, text().settings_sample_user, t.text, t.background_element.bg);
            sample(8, std::string(icons().check) + " " + std::string(text().settings_sample_tool), t.success, t.background.bg);
            if (d.row_ == 6) {
                const int half = (width - 1) / 2;
                for (int y = 9; y <= 10 && y < h - 3; ++y) {
                    for (int side = 0; side < 2; ++side) {
                        auto tone = y == 9 ? (side ? t.diff_added : t.diff_removed) : t.text;
                        tone.bg = y == 9 ? diff_background(t, side != 0) : t.background.bg;
                        const int left = x + side * (half + 1), cols = side ? width - half - 1 : half;
                        s.fill({left, y, cols, 1}, U' ', tone);
                        auto number = t.text_faint; number.bg = t.background.bg;
                        s.text(left, y, y == 9 ? "1 " : "2 ", number);
                        s.text(left + 2, y, fit_columns(y == 9 ? (side ? "+ new" : "− old") : "  code", cols - 2), tone);
                    }
                    s.text(x + half, y, "│", style(t.text_faint));
                }
            } else sample(9, text().settings_sample_added, t.diff_added, t.diff_added_bg.bg);
            sample(11, text().settings_sample_assistant, t.text, t.background.bg);
            sample(13, text().settings_sample_prompt, t.text_faint,
                   d.current_.panels == "outline" ? t.background.bg : t.background_panel.bg);
        }
        if (h > 4) {
            const auto message = d.saving_ ? text().settings_saving : d.invalid_ ? text().settings_invalid : std::string_view{};
            s.text(2, h - 3, fit_columns(message, w - 4), style(d.invalid_ ? t.error : t.text_muted));
            draw_hint(s, 2, h - 2, fit_columns(d.editing_ ? text().settings_hex_footer : text().settings_footer, w - 4), style(t.text_muted), t.text_faint);
        }
    }
};
SettingsDialog::~SettingsDialog() { close(); }
void SettingsDialog::open(Appearance a, Layout layout, std::function<bool(const Appearance&, const Layout&)> preview,
                          std::function<void(const Appearance&, const Layout&)> save, int row, std::function<void()> revert) {
    close(); original_ = current_ = std::move(a); preview_ = std::move(preview); save_ = std::move(save); revert_ = std::move(revert); row_ = row;
    original_layout_ = layout_ = std::move(layout); layout_page_ = false; column_ = 0;
    width_custom_ = layout_.width.is_number_integer();
    hidden_.clear();
    for (const auto& id : section_ids())
        if (std::ranges::find(layout_.navigator_sections, id) == layout_.navigator_sections.end() &&
            std::ranges::find(layout_.status_sections, id) == layout_.status_sections.end()) hidden_.push_back(id);
    editing_ = invalid_ = saving_ = color_accepted_ = false;
    background_custom_ = hex_color(current_.background);
    accent_custom_ = hex_color(current_.accent);
    auto view = std::make_unique<View>(*this); view_ = view.get();
    overlay_ = rt_.open_overlay(std::move(view), tui::Placement::center, {}, this);
    rt_.bind_mouse(*view_, *this);
}
void SettingsDialog::close() {
    if (!overlay_) return;
    rt_.close_overlay(overlay_); overlay_ = 0; view_ = nullptr;
}
void SettingsDialog::saved() { saving_ = false; close(); }
void SettingsDialog::failed() { saving_ = false; refresh(); }
void SettingsDialog::set_theme(const tui::ThemeTokens& t) { theme_ = t; refresh(); }
void SettingsDialog::refresh() { if (view_) view_->invalidate(); }
std::string SettingsDialog::value(int row) const {
    switch (row) {
    case 0: return current_.mode;
    case 1: return background_custom_ ? std::string(text().settings_custom) + " " + (hex_color(current_.background) ? current_.background : "") : current_.background;
    case 2:
        if (accent_custom_) return std::string(text().settings_custom) + " " + (hex_color(current_.accent) ? current_.accent : "");
        return current_.accent;
    case 3: return current_.panels;
    case 4: return current_.density;
    case 5: return current_.icons;
    case 6: return current_.diff;
    default: return current_.links;
    }
}
void SettingsDialog::cycle(int delta) {
    const auto previous = current_;
    const auto cycle_value = [delta](std::string& value, std::vector<std::string> choices) {
        const auto found = std::ranges::find(choices, value);
        const int at = found == choices.end() ? 0 : static_cast<int>(found - choices.begin());
        const int size = static_cast<int>(choices.size());
        value = choices[static_cast<std::size_t>((at + delta + size) % size)];
    };
    color_accepted_ = invalid_ = false;
    switch (row_) {
    case 0: cycle_value(current_.mode, {"auto", "dark", "light"}); break;
    case 1: {
        std::string choice = background_custom_ ? "custom" : current_.background;
        cycle_value(choice, {"solid", "transparent", "custom"}); background_custom_ = choice == "custom";
        if (!background_custom_) current_.background = choice;
        break;
    }
    case 2: {
        std::string choice = accent_custom_ ? "custom" : current_.accent;
        cycle_value(choice, {"orange", "green", "violet", "blue", "custom"});
        accent_custom_ = choice == "custom"; if (!accent_custom_) current_.accent = choice;
        break;
    }
    case 3: cycle_value(current_.panels, {"filled", "outline"}); break;
    case 4: cycle_value(current_.density, {"comfortable", "compact"}); break;
    case 5: cycle_value(current_.icons, {"unicode", "nerd", "ascii"}); break;
    case 6: cycle_value(current_.diff, {"auto", "unified"}); break;
    case 7: cycle_value(current_.links, {"on", "off"}); break;
    }
    if (!preview_(current_, layout_)) current_ = previous;
    refresh();
}
std::vector<std::string>& SettingsDialog::column(int col) {
    return col == 0 ? layout_.navigator_sections : col == 1 ? layout_.status_sections : hidden_;
}
void SettingsDialog::layout_event(const tui::Event& e) {
    const auto cycle = [](std::string& value, const std::vector<std::string>& choices, int delta) {
        const auto found = std::ranges::find(choices, value);
        const int at = found == choices.end() ? 0 : static_cast<int>(found - choices.begin());
        value = choices[(at + delta + static_cast<int>(choices.size())) % choices.size()];
    };
    if (editing_) {
        if (e.key == tui::Key::backspace) {
            if (replace_hex_) hex_.clear(); else if (!hex_.empty()) hex_.pop_back();
            replace_hex_ = false;
        } else if (!e.text.empty() && !tui::any(e.mods & tui::Mods::ctrl)) {
            if (replace_hex_) hex_.clear();
            replace_hex_ = false; hex_ += e.text;
        }
        int width = 0;
        const auto parsed = std::from_chars(hex_.data(), hex_.data() + hex_.size(), width);
        invalid_ = parsed.ec != std::errc{} || parsed.ptr != hex_.data() + hex_.size() || width < 60 || width > 240;
        if (!invalid_) { layout_.width = width; preview_(current_, layout_); }
        if (e.key == tui::Key::enter && !invalid_) { editing_ = false; color_accepted_ = true; }
        refresh(); return;
    }
    if (e.key == tui::Key::up || e.key == tui::Key::down) {
        const int delta = e.key == tui::Key::up ? -1 : 1;
        auto& ids = column(column_);
        if (row_ >= 3 && tui::any(e.mods & tui::Mods::shift)) {
            const int at = row_ - 3, next = at + delta;
            if (next >= 0 && next < static_cast<int>(ids.size())) { std::swap(ids[at], ids[next]); row_ += delta; }
        } else {
            const int size = 3 + static_cast<int>(ids.size()); row_ = (row_ + delta + size) % size;
        }
        color_accepted_ = false;
    } else if (e.key == tui::Key::left || e.key == tui::Key::right) {
        const int delta = e.key == tui::Key::left ? -1 : 1;
        if (row_ == 0) {
            auto value = width_custom_ ? std::string("custom") : layout_.width.get<std::string>();
            cycle(value, {"auto", "compact", "wide", "custom"}, delta);
            width_custom_ = value == "custom"; layout_.width = width_custom_ ? nlohmann::json(120) : nlohmann::json(value);
            color_accepted_ = false;
        } else if (row_ == 1) cycle(layout_.navigator, {"auto", "always", "hidden"}, delta);
        else if (row_ == 2) cycle(layout_.status_panel, {"right", "left", "hidden"}, delta);
        else {
            const int next = std::clamp(column_ + delta, 0, 2);
            if (next != column_) {
                auto& from = column(column_); const auto id = from[row_ - 3]; from.erase(from.begin() + row_ - 3);
                auto& to = column(next); to.push_back(id); column_ = next; row_ = 2 + static_cast<int>(to.size());
            }
        }
    } else if (e.key == tui::Key::enter) {
        if (row_ == 0 && width_custom_ && !color_accepted_) {
            hex_ = layout_.width.dump(); editing_ = replace_hex_ = true;
        } else { saving_ = true; refresh(); save_(current_, layout_); return; }
    }
    preview_(current_, layout_); refresh();
}
bool SettingsDialog::on_event(const tui::Event& e) {
    if (!active()) return false;
    if (saving_) return true;
    if (e.key == tui::Key::escape || (tui::any(e.mods & tui::Mods::ctrl) && e.text == "c")) {
        preview_(original_, original_layout_); close(); if (revert_) revert_(); return true;
    }
    if (e.key == tui::Key::tab && !editing_) {
        layout_page_ = !layout_page_; row_ = 0; invalid_ = false; refresh(); return true;
    }
    if (e.kind == tui::Event::Kind::mouse && e.mouse.press && e.mouse.button == 0 && !e.mouse.outside && e.mouse.x >= 0 && e.mouse.x < 16 && (e.mouse.y == 2 || e.mouse.y == 3)) {
        layout_page_ = e.mouse.y == 3; row_ = 0; editing_ = invalid_ = false; refresh(); return true;
    }
    if (layout_page_) { layout_event(e); return true; }
    if (editing_) {
        if (e.key == tui::Key::enter) {
            if (!hex_color(hex_)) { invalid_ = true; refresh(); return true; }
            (row_ == 1 ? current_.background : current_.accent) = hex_;
            if (preview_(current_, layout_)) { editing_ = invalid_ = false; color_accepted_ = true; }
        } else if (e.key == tui::Key::backspace) {
            if (replace_hex_) hex_.clear(); else if (!hex_.empty()) hex_.pop_back();
            replace_hex_ = false; invalid_ = !hex_color(hex_);
        } else if (!e.text.empty() && !tui::any(e.mods & tui::Mods::ctrl)) {
            if (replace_hex_) hex_.clear();
            replace_hex_ = false;
            hex_ += e.text; invalid_ = !hex_color(hex_);
        }
        refresh(); return true;
    }
    if (e.key == tui::Key::up || e.key == tui::Key::down) {
        row_ = (row_ + (e.key == tui::Key::up ? 7 : 1)) % 8;
        color_accepted_ = invalid_ = false; refresh();
    } else if (e.key == tui::Key::left || e.key == tui::Key::right) {
        cycle(e.key == tui::Key::left ? -1 : 1);
    } else if (e.key == tui::Key::enter) {
        if (!color_accepted_ && ((row_ == 1 && background_custom_) || (row_ == 2 && accent_custom_))) {
            hex_ = row_ == 1 ? current_.background : current_.accent;
            if (!hex_color(hex_)) hex_ = "#";
            editing_ = replace_hex_ = true; refresh();
        } else { saving_ = true; refresh(); save_(current_, layout_); }
    }
    return true;
}
} // namespace dagent::ui
