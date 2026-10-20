#include "ui/header_bar.hpp"
#include "ui/display.hpp"
#include "ui/strings.hpp"
#include <algorithm>
#include <format>

namespace dagent::ui {
void HeaderBar::render(tui::Surface& s) {
    const auto& t = *theme_;
    const auto background = outline ? t.background : t.background_panel;
    s.fill({0, 0, s.cols(), s.rows()}, U' ', background);
    const auto style = [&](tui::Style v) { v.bg = background.bg; return v; };
    if (outline) for (int x = 0; x < s.cols(); ++x) s.put(x, 1, icons().horizontal, style(t.border));
    const int gap = outline ? 2 : 1;
    const double percent = limit ? 100.0 * used / limit : 0;
    std::string end = sidebar ? (branch.empty() ? "" : std::string(icons().branch) + " " + branch)
                              : std::format("{:.0f}%", percent);
    // 宽度预算先留给模型/用量；超长分支与模型按显示列裁剪。
    end = fit_columns(end, std::max(0, s.cols() / 4));
    const std::string label = fit_columns(model, std::max(1, s.cols() / 3));
    bool show_mode = sidebar;
    int right = chip_width(label, outline) + (end.empty() ? 0 : gap + display_width(end));
    if (show_mode && right + chip_width(mode, outline) + gap + 12 > s.cols()) show_mode = false;
    if (show_mode) right += chip_width(mode, outline) + gap;
    const int start = std::max(0, s.cols() - right - 1);
    auto brand = style(t.primary); brand.attrs = brand.attrs | tui::Attr::bold;
    const std::string identity = std::string(icons().brand) + " " + std::string(text().brand);
    int x = s.text(1, 0, fit_columns(identity, std::max(0, start - 2)), brand);
    if (start - x >= 5) {
        x = s.text(x, 0, " / ", style(t.text_muted));
        s.text(x, 0, fit_columns(title.empty() ? text().new_session : title, start - x - 1),
               style(title.empty() ? t.text_muted : t.text));
    }
    x = start;
    if (show_mode) {
        const auto tone = mode == "unrestricted" ? t.error : mode == "plan" ? t.accent : t.text_muted;
        x = draw_chip(s, x, 0, mode, tone, t, outline, background.bg) + gap;
    }
    x = draw_chip(s, x, 0, label, t.text, t, outline, background.bg);
    if (!end.empty()) s.text(x + gap, 0, end, style(!sidebar && percent >= trigger ? t.warning : t.text_muted));
}
} // namespace dagent::ui
