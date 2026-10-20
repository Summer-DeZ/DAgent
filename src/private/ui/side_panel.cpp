#include "ui/side_panel.hpp"
#include "ui/display.hpp"
#include "ui/strings.hpp"
#include "tui/document.hpp"
#include <algorithm>
#include <charconv>
#include <ctime>
#include <format>

namespace dagent::ui {
namespace {
std::string tokens(std::size_t n) {
    if (n < 1000) return std::to_string(n);
    return n % 1000 == 0 ? std::format("{}k", n / 1000) : std::format("{:.1f}k", n / 1000.0);
}
}
void SidePanel::set_title(std::string value) { title_ = std::move(value); invalidate(); }
void SidePanel::set_context(std::size_t used, std::size_t limit) { used_ = used; limit_ = limit; invalidate(); }
void SidePanel::set_items(TodoList items) { items_ = std::move(items); has_plan_ = true; invalidate(); }
void SidePanel::set_mcp(std::vector<McpStatus> value) { mcp_ = std::move(value); invalidate(); }
void SidePanel::set_version(std::string value) { version_ = format_text(text().panel_version, value); invalidate(); }
void SidePanel::set_collapsed(bool value) { collapsed_ = value; invalidate_layout(); }
void SidePanel::set_appearance(const Appearance& a) {
    outline_ = a.panels == "outline"; compact_ = a.density == "compact"; left_ = a.sidebar == "left";
    invalidate_layout();
}
void SidePanel::start_session(std::string_view id) {
    agents_.clear(); title_.clear(); turns_ = 0; changes_.clear(); items_.clear(); has_plan_ = false;
    // 会话 ID 为 UUIDv7，前 48 位是创建时的 Unix 毫秒，恢复会话也保持原始时间。
    std::string stamp;
    for (char c : id.substr(0, 13)) if (c != '-') stamp += c;
    std::uint64_t millis = 0;
    const auto result = std::from_chars(stamp.data(), stamp.data() + stamp.size(), millis, 16);
    const std::time_t when = result.ec == std::errc{} ? static_cast<std::time_t>(millis / 1000) : std::time(nullptr);
    std::tm local{}; localtime_r(&when, &local);
    char time[6]; std::strftime(time, sizeof(time), "%H:%M", &local); started_ = time;
    invalidate();
}
void SidePanel::user_turn(const std::string& input) {
    ++turns_;
    if (title_.empty()) title_ = input.substr(0, input.find('\n'));
    invalidate();
}
void SidePanel::changed(const FileChangeView& change) {
    auto path = std::filesystem::path(change.path);
    if (path.is_absolute()) path = path.lexically_relative(root_);
    auto& count = changes_[path.string()]; count.first += change.added; count.second += change.removed;
    invalidate();
}
std::pair<int, int> SidePanel::progress() const noexcept {
    return {static_cast<int>(std::ranges::count_if(items_, [](const auto& i) { return i.state == TodoItem::State::done; })),
            static_cast<int>(items_.size())};
}
tui::Size SidePanel::measure(tui::Size a) const {
    return collapsed_ || a.cols < 80 ? tui::Size{} : tui::Size{a.cols >= 100 ? 30 : 26, a.rows};
}
void SidePanel::render(tui::Surface& s) {
    const int w = s.cols(), h = s.rows(), x = 2, width = w - 4;
    if (w < 6 || h < 3) return;
    const auto& t = *theme_;
    const auto bg = outline_ ? t.background : t.background_panel;
    const auto style = [&](tui::Style v) { v.bg = bg.bg; return v; };
    s.fill({0, 0, w, h}, U' ', bg);
    if (outline_) for (int row = 0; row < h; ++row) s.put(left_ ? w - 1 : 0, row, icons().vertical, style(t.border));
    int y = 1;
    const auto line = [&](std::string_view value, tui::Style tone) {
        if (y < h - 2) s.text(x, y++, fit_columns(value, width), style(tone));
    };
    const auto gap = [&] { if (!compact_ && y < h - 2) ++y; };
    const auto heading = [&](std::string_view label, const std::string& right = "", tui::Style tone = {}) {
        if (y >= h - 2) return;
        s.text(x, y, fit_columns(label, width - display_width(right)), style(t.text_muted));
        s.text(w - 2 - display_width(right), y++, right, style(tone));
    };
    heading(text().section_session);
    const std::string title = title_.empty() ? std::string(text().new_session) : title_;
    const auto edge = tui::wrap_next_row(title, 0, width);
    line(title.substr(0, edge.end), t.text);
    if (edge.next < title.size()) line(std::string_view(title).substr(edge.next), t.text);
    const auto metadata = format_text(turns_ == 1 ? text().session_started_one : text().session_started, started_, turns_);
    const auto metadata_edge = tui::wrap_next_row(metadata, 0, width);
    line(metadata.substr(0, metadata_edge.end), t.text_muted);
    if (metadata_edge.next < metadata.size()) line(std::string_view(metadata).substr(metadata_edge.next), t.text_muted);
    gap();
    const double percent = limit_ ? 100.0 * used_ / limit_ : 0;
    heading(text().section_context, std::format("{:.0f}%", percent), percent >= trigger_ ? t.warning : t.text_muted);
    if (y < h - 2) {
        const int filled = std::clamp(static_cast<int>(width * percent / 100), 0, width);
        for (int col = 0; col < width; ++col) s.put(x + col, y, icons().meter, style(col < filled ? t.primary : t.border));
        ++y;
    }
    line(format_text(text().context_tokens, tokens(used_), tokens(limit_)), t.text_muted);
    if (has_plan_) {
        gap(); const auto [done, total] = progress();
        heading(text().section_plan, std::format("{}/{}", done, total), t.text_muted);
        for (const auto& item : items_) {
            if (y >= h - 2) break;
            const bool done_item = item.state == TodoItem::State::done, doing = item.state == TodoItem::State::doing;
            s.text(x, y, done_item ? icons().check : doing ? icons().current : icons().circle,
                   style(done_item ? t.success : doing ? t.accent : t.text_muted));
            s.text(x + 2, y++, fit_columns(item.text, width - 2), style(doing ? t.text : t.text_muted));
        }
    }
    if (!agents_.empty()) {
        gap(); heading(text().section_agents);
        for (const auto& agent : agents_) {
            if (y >= h - 2) break;
            const bool running = agent.state == AgentState::running, failed = agent.state == AgentState::failed;
            const auto elapsed = std::format("{:.1f}s", agent.seconds);
            s.text(x, y, running ? icons().spinner[frame_] : failed ? icons().cross : icons().check,
                   style(running ? t.accent : failed ? t.error : t.success));
            s.text(x + 2, y, fit_columns(agent.name, width - display_width(elapsed) - 3), style(t.text));
            s.text(w - 2 - display_width(elapsed), y++, elapsed, style(t.text_muted));
        }
    }
    if (!changes_.empty()) {
        gap(); heading(text().section_changed);
        auto added_style = style(t.success), removed_style = style(t.error);
        added_style.attrs = added_style.attrs & ~tui::Attr::bold;
        removed_style.attrs = removed_style.attrs & ~tui::Attr::bold;
        int count = 0;
        for (const auto& [path, counts] : changes_) {
            if (count++ == 8 || y >= h - 2) break;
            const auto added = std::format("+{}", counts.first), removed = std::string(icons().minus) + std::to_string(counts.second);
            const int right = w - 2 - display_width(added) - 1 - display_width(removed);
            s.text(x, y, fit_columns(path, right - x - 1), style(t.text));
            s.text(right, y, added, added_style);
            s.text(right + display_width(added) + 1, y++, removed, removed_style);
        }
        if (changes_.size() > 8) line(format_text(text().changed_more, changes_.size() - 8), t.text_muted);
    }
    if (!mcp_.empty()) {
        gap(); heading(text().section_mcp);
        for (const auto& server : mcp_) {
            if (y >= h - 2) break;
            const bool ready = server.status == "ready", connecting = (server.status == "connecting" || server.status == "reconnecting");
            const auto state = ready ? text().mcp_ready : connecting ? text().mcp_connecting : text().mcp_failed;
            const auto tone = ready ? t.success : connecting ? t.warning : t.error;
            s.text(x, y, ready ? icons().dot : connecting ? icons().circle : icons().cross, style(tone));
            s.text(x + 2, y, fit_columns(server.name, width - display_width(state) - 3), style(t.text));
            s.text(w - 2 - display_width(state), y++, state, style(tone));
        }
    }
    s.text(x, h - 1, fit_columns(version_, width), style(t.text_muted));
}
} // namespace dagent::ui
