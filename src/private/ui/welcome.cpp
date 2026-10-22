#include "ui/welcome.hpp"
#include "ui/display.hpp"
#include "ui/strings.hpp"
#include "tui/document/document.hpp"
#include <algorithm>

namespace dagent::ui {
bool WelcomeView::on_event(const tui::Event& e) {
    if (e.kind != tui::Event::Kind::mouse || !e.mouse.press) return false;
    if (e.mouse.button == 4 || e.mouse.button == 5) { scroll(e.mouse.button == 4 ? -3 : 3); return true; }
    return false;
}
void WelcomeView::render(tui::Surface& s) {
    const auto& t = *theme_;
    s.fill({0, 0, s.cols(), s.rows()}, U' ', t.background);
    const int w = std::min(76, std::max(0, s.cols() - 4));
    if (w == 0 || s.rows() == 0) return;
    const bool compact = appearance.density == "compact";
    const int gap = compact ? 0 : 1;
    // comfortable 下各块拉开：字标与说明隔 1 行，大块之间隔 2 行，信息卡内上下各留 1 行。
    int logo_gap = gap, section = compact ? 0 : 2, card_pad = compact ? 0 : 1;
    const auto style = [&](tui::Style value) { value.bg = t.background.bg; return value; };
    const auto wrap = [](std::string_view value, int width) {
        std::vector<std::string_view> rows;
        for (size_t at = 0; at < value.size();) {
            const auto edge = tui::wrap_next_row(value, at, width);
            rows.push_back(value.substr(at, edge.end - at)); at = edge.next;
        }
        return rows;
    };

    const std::string version = std::string("v") + DAGENT_VERSION;
    const std::string subtitle_prefix = version + std::string(icons().separator);
    const auto subtitle = fit_columns(w >= display_width(subtitle_prefix) + 4
        ? subtitle_prefix + std::string(text().welcome_tagline) : version, w);
    const auto notice_rows = wrap(notice, w);
    struct Field { std::string_view key; std::string value; tui::Style tone; };
    std::vector<Field> fields{{text().welcome_project, path, t.text}};
    if (!branch.empty()) fields.push_back({text().welcome_branch, std::string(icons().branch) + " " + branch, t.text});
    fields.push_back({text().welcome_model, model, t.text});
    fields.push_back({text().welcome_mode_key, mode,
        mode == "unrestricted" ? t.error : mode == "plan" ? t.accent : t.text_muted});
    const auto core_fields = fields.size();
    if (!mcp.empty()) {
        const auto ready = std::ranges::count_if(mcp, [](const auto& item) { return item.status == "ready"; });
        const bool failed = std::ranges::any_of(mcp, [](const auto& item) { return item.status == "failed"; });
        fields.push_back({text().welcome_mcp_key, format_text(text().welcome_mcp, icons().dot, ready),
            failed ? t.error : ready == static_cast<long>(mcp.size()) ? t.success : t.warning});
    }
    if (memories) fields.push_back({text().welcome_memory, format_text(text().welcome_memories, memories), t.text});
    const std::pair<std::string_view, std::string_view> shortcuts[] = {
        {"@", text().welcome_attach}, {"$", text().welcome_skill},
        {"shift+tab", text().welcome_mode}, {"ctrl+g", text().welcome_plan},
        {"ctrl+p", text().welcome_commands}, {"/settings", text().welcome_settings}
    };
    const bool recent = has_recent(), stacked = s.cols() < 80;
    const int recent_height = recent ? static_cast<int>(sessions.size()) + 2 : 0;
    const int shortcuts_height = 1 + static_cast<int>(std::size(shortcuts));
    bool show_tagline = true, show_shortcuts = true;
    const bool small_logo = w < 30 || appearance.icons == "ascii";
    int logo_height = small_logo ? 1 : 2;
    int card_height = 0, columns_height = 0;
    const auto height = [&] {
        card_height = static_cast<int>((fields.size() + 1) / 2) + 2 + 2 * card_pad;
        columns_height = show_shortcuts
            ? (recent && stacked ? recent_height + gap + shortcuts_height : std::max(recent_height, shortcuts_height))
            : recent_height;
        const int header_height = logo_height + (show_tagline ? 1 + (logo_height ? logo_gap : 0) : 0);
        return header_height + (header_height ? section : 0) + card_height + section + columns_height
            + (notice_rows.empty() ? 0 : gap + static_cast<int>(notice_rows.size()));
    };
    content_rows = height();
    if (content_rows > s.rows()) { logo_gap = 0; section = gap; card_pad = 0; content_rows = height(); }
    if (content_rows > s.rows()) { logo_height = 0; content_rows = height(); }
    if (content_rows > s.rows()) { show_tagline = false; content_rows = height(); }
    if (content_rows > s.rows() && recent) { show_shortcuts = false; content_rows = height(); }
    if (content_rows > s.rows()) { fields.resize(core_fields); content_rows = height(); }

    tui::Surface body(w, content_rows);
    body.fill({0, 0, w, content_rows}, U' ', t.background);
    int y = 0;
    if (logo_height) {
        auto accent = style(t.primary); accent.attrs = accent.attrs | tui::Attr::bold;
        if (small_logo) {
            const auto logo = fit_columns(std::string(appearance.icons == "ascii" ? "* " : "◆ ") + std::string(text().brand), w);
            body.text((w - display_width(logo)) / 2, y++, logo, accent);
        } else {
            for (const auto row : wrap(text().welcome_logo, w))
                body.text((w - display_width(row)) / 2, y++, row, accent);
        }
    }
    if (show_tagline) {
        if (y) y += logo_gap;
        const int x = (w - display_width(subtitle)) / 2;
        body.text(x, y, subtitle, style(t.text_muted));
        const auto separator = subtitle.find(icons().separator);
        if (separator != std::string_view::npos)
            body.text(x + display_width(std::string_view(subtitle).substr(0, separator)), y, icons().separator, style(t.text_faint));
        ++y;
    }
    if (y) y += section;
    draw_frame(body, {0, y, w, card_height}, style(t.border));
    const int field_width = std::max(0, (w - 8) / 2);
    int key_width = 0;
    for (const auto& field : fields) key_width = std::max(key_width, display_width(field.key));
    for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
        const auto& field = fields[i];
        const int x = 3 + (i % 2) * (field_width + 2), row = y + 1 + card_pad + i / 2;
        body.text(x, row, fit_columns(field.key, std::min(key_width, field_width)), style(t.text_muted));
        body.text(x + key_width + 2, row, fit_columns(field.value, field_width - key_width - 2), style(field.tone));
    }
    y += card_height + section;

    int selection_y = -1;
    if (recent) {
        const int width = std::min(42, w), x = stacked || !show_shortcuts ? (w - width) / 2 : 0;
        body.text(x, y, fit_columns(text().welcome_recent, width), style(t.text_muted));
        for (int i = 0; i < static_cast<int>(sessions.size()); ++i) {
            const int row = y + 1 + i;
            const bool current = selected == i;
            auto label = style(t.text), age = style(t.text_muted), marker = style(t.primary);
            if (current) {
                body.fill({x, row, width, 1}, U' ', t.background_selected);
                label.bg = age.bg = marker.bg = t.background_selected.bg; selection_y = row;
            }
            body.text(x, row, current ? icons().bar : " ", marker);
            const auto age_text = fit_columns(sessions[i].age, width / 3);
            const int age_x = x + width - display_width(age_text);
            body.text(x + 2, row, fit_columns(sessions[i].title, age_x - x - 3), label);
            body.text(age_x, row, age_text, age);
        }
        const auto hint = format_text(text().welcome_recent_hint, icons().up, icons().down, icons().separator);
        body.text(x, y + recent_height - 1, fit_columns(hint, width), style(t.text_faint));
    }
    if (show_shortcuts) {
        const int shortcuts_width = std::min(31, w);
        const int shortcuts_x = recent && !stacked ? w - shortcuts_width : (w - shortcuts_width) / 2;
        int shortcuts_y = y + (recent && stacked ? recent_height + gap : 0);
        body.text(shortcuts_x, shortcuts_y++, fit_columns(text().welcome_shortcuts, shortcuts_width), style(t.text_muted));
        for (const auto& [key, description] : shortcuts) {
            body.text(shortcuts_x, shortcuts_y, fit_columns(key, std::min(9, shortcuts_width)), style(t.text));
            body.text(shortcuts_x + 12, shortcuts_y++, fit_columns(description, std::max(0, shortcuts_width - 12)), style(t.text_muted));
        }
    }
    y += columns_height;
    if (!notice_rows.empty()) {
        y += gap;
        for (const auto row : notice_rows) body.text(0, y++, row, style(t.error));
    }

    top = std::clamp(top, 0, std::max(0, content_rows - s.rows()));
    if (reveal_selection && selection_y >= 0) {
        if (selection_y < top) top = selection_y;
        else if (selection_y >= top + s.rows()) top = selection_y - s.rows() + 1;
    }
    reveal_selection = false;
    const int offset_x = (s.cols() - w) / 2, offset_y = std::max(0, (s.rows() - content_rows) / 2);
    for (int row = 0; row + offset_y < s.rows() && row + top < content_rows; ++row)
        for (int col = 0; col < w;) {
            const auto cell = body.at(col, row + top);
            if (cell.width) s.put(offset_x + col, offset_y + row, cell.grapheme(), cell.style);
            col += std::max(1, static_cast<int>(cell.width));
        }
}
} // namespace dagent::ui
