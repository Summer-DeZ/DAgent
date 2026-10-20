#include "ui/settings_dialog.hpp"
#include "ui/display.hpp"
#include "ui/strings.hpp"
#include <algorithm>

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
        if (nav > 2) s.text(2, 2, text().settings_section, style(t.primary));
        const int end = preview ? w - 29 : w - 2;
        const int row_width = std::max(1, end - nav);
        const std::string_view names[] = {text().settings_theme, text().settings_mode, text().settings_background,
            text().settings_accent, text().settings_panels, text().settings_density, text().settings_icons, text().settings_sidebar};
        const int room = std::max(1, h - 6), first = std::max(0, d.row_ - room + 1);
        for (int i = first; i < 8 && i - first < room; ++i) {
            const int y = 2 + i - first;
            auto normal = style(t.text), muted = style(t.text_muted);
            if (i == d.row_) {
                s.fill({nav, y, row_width, 1}, U' ', t.background_element);
                normal.bg = muted.bg = t.background_element.bg;
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
            sample(9, text().settings_sample_added, t.diff_added, t.background.bg);
            sample(11, text().settings_sample_assistant, t.text, t.background.bg);
            sample(13, text().settings_sample_prompt, t.text_muted,
                   d.current_.panels == "outline" ? t.background.bg : t.background_panel.bg);
        }
        if (h > 4) {
            const auto message = d.saving_ ? text().settings_saving : d.invalid_ ? text().settings_invalid : std::string_view{};
            s.text(2, h - 3, fit_columns(message, w - 4), style(d.invalid_ ? t.error : t.text_muted));
            s.text(2, h - 2, fit_columns(d.editing_ ? text().settings_hex_footer : text().settings_footer, w - 4), style(t.text_muted));
        }
    }
};
SettingsDialog::~SettingsDialog() { close(); }
void SettingsDialog::open(Appearance a, std::function<bool(const Appearance&)> preview,
                          std::function<void(const Appearance&)> save, int row, std::function<void()> revert) {
    close(); original_ = current_ = std::move(a); preview_ = std::move(preview); save_ = std::move(save); revert_ = std::move(revert); row_ = row;
    editing_ = invalid_ = saving_ = color_accepted_ = false;
    background_custom_ = hex_color(current_.background);
    accent_custom_ = !current_.accent.empty() && current_.accent != "#EC5B2B" && current_.accent != "#6ba1e6" &&
                     current_.accent != "#c792ea" && current_.accent != "#7fd17f";
    auto view = std::make_unique<View>(*this); view_ = view.get();
    overlay_ = rt_.open_overlay(std::move(view), tui::Placement::center, {}, this);
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
    case 0: return current_.theme;
    case 1: return current_.mode;
    case 2: return background_custom_ ? std::string(text().settings_custom) + " " + (hex_color(current_.background) ? current_.background : "") : current_.background;
    case 3:
        if (accent_custom_) return std::string(text().settings_custom) + " " + current_.accent;
        if (current_.accent.empty()) return std::string(text().settings_default);
        if (current_.accent == "#EC5B2B") return "orange";
        if (current_.accent == "#6ba1e6") return "blue";
        if (current_.accent == "#c792ea") return "violet";
        if (current_.accent == "#7fd17f") return "green";
        return current_.accent;
    case 4: return current_.panels;
    case 5: return current_.density;
    case 6: return current_.icons;
    default: return current_.sidebar;
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
    case 0: {
        std::vector<std::string> choices{"dagent", "midnight", "paper"};
        if (std::ranges::find(choices, original_.theme) == choices.end()) choices.push_back(original_.theme);
        cycle_value(current_.theme, std::move(choices)); break;
    }
    case 1: cycle_value(current_.mode, {"auto", "dark", "light"}); break;
    case 2: {
        std::string choice = background_custom_ ? "custom" : current_.background;
        cycle_value(choice, {"solid", "transparent", "custom"}); background_custom_ = choice == "custom";
        if (!background_custom_) current_.background = choice;
        break;
    }
    case 3: {
        std::string choice = accent_custom_ ? "custom" : current_.accent;
        cycle_value(choice, {"", "#EC5B2B", "#6ba1e6", "#c792ea", "#7fd17f", "custom"});
        accent_custom_ = choice == "custom"; if (!accent_custom_) current_.accent = choice;
        break;
    }
    case 4: cycle_value(current_.panels, {"filled", "outline"}); break;
    case 5: cycle_value(current_.density, {"comfortable", "compact"}); break;
    case 6: cycle_value(current_.icons, {"unicode", "nerd", "ascii"}); break;
    case 7: cycle_value(current_.sidebar, {"right", "left", "hidden"}); break;
    }
    if (!preview_(current_)) current_ = previous;
    refresh();
}
bool SettingsDialog::on_event(const tui::Event& e) {
    if (!active()) return false;
    if (saving_) return true;
    if (e.key == tui::Key::escape || (tui::any(e.mods & tui::Mods::ctrl) && e.text == "c")) {
        preview_(original_); if (revert_) revert_(); close(); return true;
    }
    if (editing_) {
        if (e.key == tui::Key::enter) {
            if (!hex_color(hex_)) { invalid_ = true; refresh(); return true; }
            (row_ == 2 ? current_.background : current_.accent) = hex_;
            if (preview_(current_)) { editing_ = invalid_ = false; color_accepted_ = true; }
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
        if (!color_accepted_ && ((row_ == 2 && background_custom_) || (row_ == 3 && accent_custom_))) {
            hex_ = row_ == 2 ? current_.background : current_.accent;
            if (!hex_color(hex_)) hex_ = "#";
            editing_ = replace_hex_ = true; refresh();
        } else { saving_ = true; refresh(); save_(current_); }
    }
    return true;
}
} // namespace dagent::ui
