#include "ui/welcome.hpp"
#include "ui/display.hpp"
#include "ui/strings.hpp"
#include "tui/document.hpp"
#include <algorithm>

namespace dagent::ui {
bool WelcomeView::on_event(const tui::Event& e) {
    if (e.kind != tui::Event::Kind::mouse || !e.mouse.press) return false;
    if (e.mouse.button == 4 || e.mouse.button == 5) { scroll(e.mouse.button == 4 ? -3 : 3); return true; }
    return false;
}
void WelcomeView::render(tui::Surface& s) {
    const auto& t = *theme_;
    tui::Surface body(s.cols(), 64);
    body.fill({0, 0, body.cols(), body.rows()}, U' ', t.background);
    const int w = body.cols();
    const int gap = appearance.density == "compact" ? 0 : 1;
    const bool outline = appearance.panels == "outline";
    int y = 0;
    if (w < 60 || appearance.icons == "ascii") {
        body.text(0, y++, std::string(icons().brand) + " " + std::string(text().brand), t.primary);
    } else {
        std::string_view logo = text().welcome_logo;
        while (!logo.empty()) {
            const auto end = logo.find('\n');
            body.text(0, y++, logo.substr(0, end), t.primary);
            if (end == std::string_view::npos) break;
            logo.remove_prefix(end + 1);
        }
    }
    y += gap;
    int x = 0;
    auto chip = [&](std::string value, tui::Style tone) {
        if (value.empty()) return;
        value = fit_columns(value, w - (outline ? 0 : 2));
        if (x && x + chip_width(value, outline) > w) { x = 0; ++y; }
        x = draw_chip(body, x, y, value, tone, t, outline, t.background.bg) + (outline ? 2 : 1);
    };
    chip(path, t.text);
    if (!branch.empty()) chip(std::string(icons().branch) + " " + branch, t.text_muted);
    chip(model, t.text);
    chip(mode, mode == "unrestricted" ? t.error : mode == "plan" ? t.accent : t.text_muted);
    if (!mcp.empty()) {
        const auto ready = std::ranges::count_if(mcp, [](const auto& item) { return item.status == "ready"; });
        const bool failed = std::ranges::any_of(mcp, [](const auto& item) { return item.status == "failed"; });
        chip(format_text(text().welcome_mcp, icons().dot, ready, mcp.size()),
             failed ? t.error : ready == static_cast<long>(mcp.size()) ? t.success : t.warning);
    }
    if (memories) chip(format_text(text().welcome_memories, memories), t.text_muted);
    ++y;
    int selection_y = -1;
    if (!sessions.empty()) {
        y += gap;
        body.text(0, y++, text().welcome_recent, t.text_muted);
        for (int i = 0; i < static_cast<int>(sessions.size()); ++i, ++y) {
            const bool current = selected == i;
            auto label = t.text, age = t.text_muted, marker = t.primary;
            if (current) {
                body.fill({0, y, w, 1}, U' ', t.background_element);
                label.bg = age.bg = marker.bg = t.background_element.bg; selection_y = y;
            }
            body.text(0, y, current ? icons().bar : " ", marker);
            const auto right = fit_columns(sessions[i].age, std::max(0, w / 3));
            const int col = w - display_width(right);
            body.text(2, y, fit_columns(sessions[i].title, col - 3), label);
            body.text(col, y, right, age);
        }
        body.text(2, y++, fit_columns(text().welcome_all_sessions, w - 2), t.text_muted);
    }
    y += gap;
    body.text(0, y++, text().welcome_tips, t.text_muted);
    const std::pair<std::string_view, std::string_view> tips[] = {
        {"@", text().welcome_attach}, {"$", text().welcome_skill},
        {"shift+tab", text().welcome_mode}, {"ctrl+g", text().welcome_plan},
        {"/settings", text().welcome_settings}
    };
    for (const auto& [key, description] : tips) {
        const int col = body.text(0, y, key, t.text);
        body.text(col + 1, y++, fit_columns(description, w - col - 1), t.text_muted);
    }
    if (mode == "unrestricted") {
        y += gap;
        const auto notice = text().permission_unrestricted_body;
        for (size_t at = 0; at < notice.size();) {
            const auto edge = tui::wrap_next_row(notice, at, std::max(1, w));
            body.text(0, y++, notice.substr(at, edge.end - at), t.error); at = edge.next;
        }
    }
    top = std::clamp(top, 0, std::max(0, y - s.rows()));
    if (reveal_selection && selection_y >= 0) {
        if (selection_y < top) top = selection_y;
        else if (selection_y >= top + s.rows()) top = selection_y - s.rows() + 1;
        reveal_selection = false;
    }
    s.fill({0, 0, s.cols(), s.rows()}, U' ', t.background);
    for (int row = 0; row < s.rows() && row + top < body.rows(); ++row)
        for (int col = 0; col < w;) {
            const auto cell = body.at(col, row + top);
            if (cell.width) s.put(col, row, cell.grapheme(), cell.style);
            col += std::max(1, static_cast<int>(cell.width));
        }
}
} // namespace dagent::ui
