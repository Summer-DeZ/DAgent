#include "ui/page/side_panel.hpp"
#include "base/platform.hpp"
#include "ui/style/display.hpp"
#include "ui/style/strings.hpp"
#include "tui/document/document.hpp"
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
    outline_ = a.panels == "outline"; compact_ = a.density == "compact";
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
    std::tm local{}; base::local_time(when, local);
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
    return collapsed_ ? tui::Size{} : tui::Size{columns, a.rows};
}
bool SidePanel::on_event(const tui::Event& e) {
    if (e.kind == tui::Event::Kind::mouse) {
        if (e.mouse.button == 0 && e.mouse.press && !e.mouse.motion) {
            const int row = e.mouse.y - 1 + top_;
            auto found = std::ranges::find(selectable_, row);
            if (found != selectable_.end()) {
                selected_ = static_cast<int>(found - selectable_.begin());
                if (focus) focus();
                const auto action = rows_[row].row.activate;
                if (action) action();
                invalidate();
            }
        } else if (e.mouse.press && (e.mouse.button == 4 || e.mouse.button == 5)) {
            top_ = std::clamp(top_ + (e.mouse.button == 4 ? -3 : 3), 0,
                              std::max(0, static_cast<int>(rows_.size()) - rect().h + 2));
            invalidate();
        }
        return true;
    }
    if (e.kind != tui::Event::Kind::key && e.kind != tui::Event::Kind::text && e.kind != tui::Event::Kind::paste) return false;
    if (e.key == tui::Key::escape) { if (back) back(); return true; }
    if (tui::any(e.mods & tui::Mods::ctrl)) return false;
    const int count = static_cast<int>(selectable_.size());
    if (count && (e.key == tui::Key::up || e.key == tui::Key::down)) {
        selected_ = (selected_ + (e.key == tui::Key::up ? count - 1 : 1)) % count;
    } else if (count && e.key == tui::Key::tab) {
        const auto section = rows_[selectable_[selected_]].section;
        for (int i = 1; i <= count; ++i) {
            const int next = (selected_ + i) % count;
            if (rows_[selectable_[next]].section != section) { selected_ = next; break; }
        }
    } else if (count && e.key == tui::Key::enter) {
        const auto action = rows_[selectable_[selected_]].row.activate;
        if (action) action();
    }
    if (count) {
        const int row = selectable_[selected_], room = std::max(1, rect().h - 2);
        if (row < top_) top_ = row;
        if (row >= top_ + room) top_ = row - room + 1;
    }
    invalidate(); return true;
}
void SidePanel::render(tui::Surface& s) {
    const int w = s.cols(), h = s.rows(), x = 2, width = w - 4;
    if (w < 6 || h < 3) return;
    const auto& t = *theme_;
    const auto& data = source ? *source : *this;
    const auto bg = outline_ ? t.background : t.background_panel;
    const auto style = [&](tui::Style v) { v.bg = bg.bg; return v; };
    s.fill({0, 0, w, h}, U' ', bg);
    if (outline_) for (int y = 0; y < h; ++y) s.put(left ? w - 1 : 0, y, icons().vertical, style(t.border));
    rows_.clear(); selectable_.clear();
    const auto row = [&](std::string id, Row value, bool heading = false) {
        if (!heading && !value.left.empty()) selectable_.push_back(static_cast<int>(rows_.size()));
        rows_.push_back({std::move(value), std::move(id), heading});
    };
    auto visible = order;
    std::erase_if(visible, [&](const auto& id) {
        if (id == "session" || id == "context" || id == "sessions" || id == "git") return false;
        if (id == "plan") return !data.has_plan_;
        if (id == "changed") return data.changes_.empty();
        if (id == "running_agents") return data.agents_.empty();
        if (id == "mcp") return data.mcp_.empty();
        const auto found = data.sections.find(id);
        return found == data.sections.end() || found->second.rows.empty();
    });
    const double percent = data.limit_ ? 100.0 * data.used_ / data.limit_ : 0;
    const int filled = std::clamp(static_cast<int>(width * percent / 100), 0, width);
    for (const auto& id : visible) {
        if (!rows_.empty() && !compact_) row(id, {}, true);
        Section section;
        if (const auto found = data.sections.find(id); found != data.sections.end()) section = found->second;
        if (id == "session") {
            section.rows.push_back({data.title_.empty() ? std::string(text().new_session) : data.title_, {}, {}, false, 0});
            section.rows.push_back({format_text(data.turns_ == 1 ? text().session_started_one : text().session_started, data.started_, data.turns_), {}, {}, false, 'f'});
        } else if (id == "context") {
            section.right = std::format("{:.0f}%", percent);
            section.rows.push_back({format_text(text().context_tokens, tokens(data.used_), tokens(data.limit_)), {}, {}, false, 'f'});
            std::string meter;
            for (int i = 0; i < width; ++i) meter += icons().meter;
            section.rows.insert(section.rows.begin(), {meter, {}, {}, false, percent >= data.trigger_ ? 'M' : 'p'});
        } else if (id == "plan") {
            const auto [done, total] = data.progress(); section.right = std::format("{}/{}", done, total);
            for (const auto& item : data.items_) section.rows.push_back({std::string(item.state == TodoItem::State::done ? icons().done : icons().todo) + " " + item.text, {}, {}, false, item.state == TodoItem::State::doing ? 'p' : 'f'});
        } else if (id == "running_agents") {
            for (const auto& agent : data.agents_) {
                const bool running = agent.state == AgentState::running, failed = agent.state == AgentState::failed;
                section.rows.push_back({std::string(running ? icons().spinner[data.frame_] : failed ? icons().cross : icons().check) + " " + agent.name,
                    std::format("{:.1f}s", agent.seconds), {}, false, failed ? 'D' : running ? 'p' : 'A'});
            }
        } else if (id == "changed") {
            int count = 0;
            for (const auto& [path, counts] : data.changes_) {
                if (count++ == 8) break;
                section.rows.push_back({path, std::format("+{} −{}", counts.first, counts.second), {}, false, 0});
            }
            if (data.changes_.size() > 8) section.rows.push_back({format_text(text().changed_more, data.changes_.size() - 8), {}, {}, false, 'f'});
        } else if (id == "mcp") {
            for (const auto& server : data.mcp_) {
                const bool ready = server.status == "ready", connecting = server.status == "connecting" || server.status == "reconnecting";
                section.rows.push_back({std::string(ready ? icons().dot : connecting ? icons().circle : icons().cross) + " " + server.name,
                    std::string(ready ? text().mcp_ready : connecting ? text().mcp_connecting : text().mcp_failed), {}, false, ready ? 'A' : connecting ? 'M' : 'D'});
            }
        }
        row(id, {std::string(section_name(id)), section.right, {}, false, 0}, true);
        if (id == "sessions" && !section.rows.empty()) {
            // Leave room for the remaining section headings and their first row.
            const int reserve = 3 * static_cast<int>(visible.end() - std::ranges::find(visible, id) - 1);
            const int room = std::max(1, h - 6 - static_cast<int>(rows_.size()) - reserve);
            const int count = static_cast<int>(section.rows.size()) - 1;
            for (int i = 0; i < std::min(count, room); ++i) row(id, section.rows[i]);
            if (count > room) row(id, {format_text(text().navigator_all_sessions, count), {}, data.all_sessions, false, 'f'});
            row(id, section.rows.back());
        } else {
            for (auto& item : section.rows) row(id, std::move(item));
        }
    }
    selected_ = std::clamp(selected_, 0, std::max(0, static_cast<int>(selectable_.size()) - 1));
    top_ = std::clamp(top_, 0, std::max(0, static_cast<int>(rows_.size()) - h + 2));
    const int selection = selectable_.empty() ? -1 : selectable_[selected_];
    for (int y = 1, i = top_; y < h - 1 && i < static_cast<int>(rows_.size()); ++y, ++i) {
        const auto& item = rows_[i]; const auto& value = item.row;
        auto tone = style(item.heading ? t.text_muted : value.status == 'f' ? t.text_faint : value.status == '?' ? t.text_muted :
                          value.status == 'M' ? t.warning : value.status == 'A' ? t.success : value.status == 'D' ? t.error : value.status == 'p' ? t.primary : t.text);
        auto muted = style(value.status == 'f' ? t.text_faint : t.text_muted);
        if (!item.heading && ((focused && i == selection) || value.current)) {
            s.fill({1, y, w - 2, 1}, U' ', t.background_selected);
            if (value.status == 'f') tone = muted = t.text_muted;
            tone.bg = muted.bg = t.background_selected.bg;
            if (focused && i == selection) {
                auto bar = t.primary; bar.bg = tone.bg; s.text(0, y, icons().bar, bar);
            }
        }
        const auto right = fit_columns(value.right, std::max(0, width - 4));
        const int right_width = display_width(right);
        const auto left_text = fit_columns(value.left, width - (right.empty() ? 0 : right_width + 1));
        if (item.section == "context" && !item.heading && (value.status == 'p' || value.status == 'M')) {
            auto unused = t.border; unused.bg = tone.bg;
            for (int col = 0; col < width; ++col) s.put(x + col, y, icons().meter, col < filled ? tone : unused);
        } else if (item.section == "git" && (value.status == 'M' || value.status == 'A' || value.status == 'D' || value.status == '?')) {
            auto path_style = t.text; path_style.bg = tone.bg;
            s.text(x, y, left_text, path_style);
            s.text(x, y, value.left.substr(0, 1), tone);
        } else s.text(x, y, left_text, tone);
        const auto split = right.find(' ');
        if (!item.heading && (item.section == "git" || item.section == "changed") && right.starts_with('+') && split != std::string::npos) {
            auto added = t.success, removed = t.error; added.bg = removed.bg = tone.bg;
            s.text(w - 2 - right_width, y, right.substr(0, split), added);
            s.text(w - 2 - right_width + static_cast<int>(split) + 1, y, right.substr(split + 1), removed);
        } else s.text(w - 2 - right_width, y, right, muted);
    }
    if (!source) s.text(x, h - 1, fit_columns(data.version_, width), style(t.text_faint));
}
} // namespace dagent::ui
