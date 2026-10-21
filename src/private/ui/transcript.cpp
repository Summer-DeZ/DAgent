#include "ui/transcript.hpp"
#include "ui/display.hpp"
#include "ui/strings.hpp"
#include "ui/appearance.hpp"
#include "ui/center.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

#include "base/overloaded.hpp"
#include "base/text.hpp"
#include "tui/grapheme.hpp"

namespace dagent::ui {
namespace {
// 核心生成的“未执行/被拒绝”结果文本前缀（agent/conversation.hpp 的 texts）：
// UI 只据此决定卡片显示为 stopped，不改变结果本身。
constexpr std::string_view kDenied =
    "The user denied this call. Do not work around it; wait for further instructions.";
constexpr std::string_view kDeniedWithFeedback = "The user denied this call and said: ";
constexpr std::string_view kPriorDenied =
    "An earlier call in the same batch was denied; this call was not executed.";
constexpr std::string_view kPolicyDenied = "The permission policy denied this call: ";
constexpr std::string_view kApprovalUnavailable = "This call requires user approval: ";
constexpr std::string_view kToolLimit = "This turn hit the tool call limit (";

std::string fit(std::string_view value, int columns) {
    if (columns <= 0) return {};
    std::string result;
    int used = 0;
    tui::unicode::Grapheme g;
    while (tui::unicode::next_grapheme(value, g)) {
        if (used + g.width > columns) break;
        result.append(g.bytes); used += g.width;
    }
    return result;
}

std::string clean_field(std::string value) {
    std::ranges::replace(value, '\t', ' ');
    std::ranges::replace(value, '\n', ' ');
    return value;
}

std::string task_stats(std::size_t tools, double seconds) {
    const auto& text = ui::text();
    return format_text(text.card_task_stats, tools, tools == 1 ? text.card_task_tool : text.card_task_tools,
                       seconds, icons().separator);
}

std::string trim_blank_lines(std::string_view value) {
    while (!value.empty()) {
        const auto end = value.find('\n');
        if (value.substr(0, end).find_first_not_of(" \t\r") != std::string_view::npos) break;
        value.remove_prefix(end == std::string_view::npos ? value.size() : end + 1);
    }
    while (!value.empty()) {
        const auto start = value.rfind('\n');
        if (value.substr(start == std::string_view::npos ? 0 : start + 1).find_first_not_of(" \t\r") != std::string_view::npos) break;
        value = value.substr(0, start == std::string_view::npos ? 0 : start);
    }
    return std::string(value);
}

// 文本结果沿用 Task 的源前缀约定，使 measure 与 render 使用相同的缩进。
std::string result_prefix() { return "  " + std::string(icons().result) + " "; }
int result_indent() { return display_width(result_prefix()); }
void hang_result(tui::Line& line, bool first, const tui::ThemeTokens& theme) {
    const auto prefix = first ? result_prefix() : std::string(static_cast<size_t>(result_indent()), ' ');
    line.spans.insert(line.spans.begin(), {prefix, theme.text_faint, tui::k_no_src});
    line.width += result_indent();
}

// 用户原文独立使用 output 槽位，时间放在 meta，避免复制消息时带出显示时间。
class UserRenderer final : public tui::BlockRenderer {
public:
    tui::WrapResult measure(std::string_view source, size_t from, int width) const override {
        return tui::wrap_measure_from(source, from, std::max(1, width - 8));
    }
    size_t render(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                  size_t from, size_t valid, std::vector<tui::Line>& out) const override {
        const auto count = tui::TextRenderer().render(block, std::max(1, width - 8), theme, from, valid, out);
        const auto time = std::string_view(block.meta).substr(5);
        for (size_t i = valid; i < count; ++i) {
            auto& line = out[i];
            for (auto& span : line.spans) span.style.bg = theme.background_element.bg;
            auto bar = theme.text_muted; bar.bg = theme.background_element.bg;
            line.spans.insert(line.spans.begin(), {i == 0 ? std::string(icons().user) + " " : "  ", bar, tui::k_no_src});
            auto muted = theme.text_muted; muted.bg = theme.background_element.bg;
            const auto stamp = i == 0 && width >= 8 ? time : std::string_view{};
            line.spans.push_back({std::string(static_cast<size_t>(std::max(0, width - line.width - 2 - display_width(stamp))), ' '), muted, tui::k_no_src});
            if (!stamp.empty()) line.spans.push_back({std::string(stamp), muted, tui::k_no_src});
            line.width = width;
        }
        return count;
    }
};

class ChatRenderer final : public tui::BlockRenderer {
public:
    ChatRenderer(const int& frame, const tui::Document& doc) : frame_(frame), doc_(doc) {}
    tui::WrapResult measure(std::string_view source, size_t from, int width) const override {
        if (from >= source.size()) return {};
        if (source.starts_with('\x1c')) {
            tui::Block block; block.source = source;
            std::vector<tui::Line> lines;
            render_task(block, width, tui::dark_theme(), lines);
            return {lines.size(), 0, 0};
        }
        if (source.starts_with('\x1a') || source.starts_with('\x19') || source.starts_with('\x18')) {
            tui::Block block; block.source = source;
            std::vector<tui::Line> lines;
            render_details(block, width, tui::dark_theme(), lines);
            return {lines.size(), 0, 0};
        }
        if (source.starts_with('\x1d') || source.starts_with('\x1b'))
            return {tui::wrap_measure_from(source.substr(1), 0,
                std::max(1, width - (source.front() == '\x1b' ? 4 : result_indent()))).rows, 0, 0};
        if (const std::size_t marker = source.find("BANNER_END\n"); marker != std::string_view::npos) {
            std::string visible(source);
            if (width < 60 || icons().bar == "|") visible.erase(0, marker + 11);
            else visible.erase(marker, 11);
            return tui::wrap_measure_from(visible, from, std::max(1, width - 2));
        }
        if (source.starts_with('\x1f')) {
            const auto& body = *doc_.find(std::stoull(std::string(source.substr(1))));
            return {body.row_count > body.collapsed_rows ? size_t{1} : size_t{0}, 0, 0};
        }
        if (source.starts_with('\x1e')) return {1, 1, source.size()};
        return tui::wrap_measure_from(source, from, std::max(1, width - 2));
    }

    size_t render(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                  size_t from, size_t valid, std::vector<tui::Line>& out) const override {
        if (block.source.empty()) { out.clear(); return 0; }
        if (block.source.starts_with('\x1c')) return render_task(block, width, theme, out);
        if (block.source.starts_with('\x1a') || block.source.starts_with('\x19') || block.source.starts_with('\x18'))
            return render_details(block, width, theme, out);
        if (block.source.starts_with('\x1d') || block.source.starts_with('\x1b'))
            return render_body(block, width, theme, out);
        if (block.meta.starts_with("tool.")) return render_tool(block, width, theme, out);
        if (block.meta == "banner") return render_banner(block, width, theme, from, valid, out);

        auto style = block.meta.starts_with("msg.footer") ? theme.text_faint : theme.text_muted;
        std::string first = "  ";
        auto prefix_style = style;
        if (block.meta == "thought" || block.meta == "thought.live") {
            style = theme.text_muted; style.attrs = style.attrs | tui::Attr::italic;
            first = std::string(icons().thought) + " ";
            prefix_style = block.meta == "thought.live" ? theme.accent : theme.text_muted;
        } else if (block.meta == "msg.footer.error") style = theme.error;
        const size_t count = tui::TextRenderer().render(block, std::max(1, width - 2), theme, from, valid, out);
        for (size_t i = valid; i < count; ++i) {
            for (auto& span : out[i].spans) span.style = style;
            out[i].spans.insert(out[i].spans.begin(), {i == 0 ? first : "  ", prefix_style, tui::k_no_src});
            out[i].width += 2;
        }
        return count;
    }

private:
    const int& frame_;
    const tui::Document& doc_;
    size_t render_task(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                       std::vector<tui::Line>& out) const {
        out.clear();
        const int room = std::max(1, width - result_indent());
        size_t at = 1;
        for (; at < block.source.size() && block.source[at] == '\x1e';) {
            const auto end = std::min(block.source.find('\n', at), block.source.size());
            tui::Block row; row.source = block.source.substr(at, end - at);
            std::vector<tui::Line> lines;
            const char state = row.source[1]; row.source.erase(1, 1);
            row.meta = state == 'R' ? "tool.running" : state == 'E' ? "tool.error" : "tool.done";
            render_tool(row, room, theme, lines, true);
            for (auto& line : lines) {
                line.offset += at;
                for (auto& span : line.spans) if (span.src != tui::k_no_src) span.src += at + 1;
                hang_result(line, out.empty(), theme);
                out.push_back(std::move(line));
            }
            at = end + 1;
        }
        if (at < block.source.size() && block.source[at] == '\n') {
            tui::Line line; line.offset = at++;
            hang_result(line, out.empty(), theme);
            out.push_back(std::move(line));
        }
        // 与 assistant 共用 Markdown 分块及渲染器，保留 Task 单一主体的折叠和源偏移。
        tui::Document result;
        tui::MarkdownStream markdown(result);
        markdown.feed(std::string_view(block.source).substr(std::min(at, block.source.size())));
        markdown.finish();
        const auto end = result.append_block(tui::BlockKind::text);
        auto body_theme = theme;
        body_theme.text = block.meta == "tool.body.error" ? theme.error : theme.text_muted;
        for (uint64_t id = 1; id < end; ++id) {
            tui::Block part = *result.find(id);
            if (part.source.empty()) continue;
            // 分块原文均从逻辑行首开始；跳过围栏语言等行内的同名片段。
            auto offset = block.source.find(part.source, at);
            while (offset > at && block.source[offset - 1] != '\n')
                offset = block.source.find(part.source, offset + 1);
            const auto& renderer = result.renderer(part.kind);
            part.row_count = renderer.measure(part.source, 0, room).rows;
            std::vector<tui::Line> lines;
            const auto count = renderer.render(part, room, body_theme, 0, 0, lines);
            for (size_t i = 0; i < count; ++i) {
                auto& line = lines[i];
                line.offset += offset;
                for (auto& span : line.spans) if (span.src != tui::k_no_src) span.src += offset;
                hang_result(line, out.empty(), theme);
                out.push_back(std::move(line));
            }
            at = offset + part.source.size();
        }
        if (block.collapsed && out.size() > block.collapsed_rows) out.resize(block.collapsed_rows);
        return out.size();
    }
    size_t render_body(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                       std::vector<tui::Line>& out) const {
        const bool thought = block.source.front() == '\x1b';
        const int indent = thought ? 4 : result_indent();
        tui::Block body = block; body.source.erase(0, 1);
        auto style = block.meta == "tool.body.error" ? theme.error : theme.text_muted;
        if (thought) style.attrs = style.attrs | tui::Attr::italic;
        out.clear();
        const auto count = tui::TextRenderer().render(body, std::max(1, width - indent), theme, 0, 0, out);
        for (auto& line : out) {
            ++line.offset;
            for (auto& span : line.spans) { span.style = style; if (span.src != tui::k_no_src) ++span.src; }
            if (thought) {
                line.spans.insert(line.spans.begin(), {"  " + std::string(icons().vertical) + " ", theme.text_faint, tui::k_no_src});
                line.width += indent;
            } else hang_result(line, &line == &out.front(), theme);
        }
        return count;
    }

    // Ask、Plan 与系统提示的布局同时用于计数和绘制，保持窄屏折行一致。
    size_t render_details(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                          std::vector<tui::Line>& out) const {
        out.clear();
        const auto append = [&](std::string_view value, size_t offset, int indent, tui::Style style,
                                std::string prefix, tui::Style prefix_style) {
            size_t at = 0;
            do {
                const auto edge = tui::wrap_next_row(value, at, std::max(1, width - indent));
                tui::Line line; line.offset = offset + at;
                line.spans.push_back({at == 0 ? prefix : std::string(static_cast<size_t>(indent), ' '), prefix_style, tui::k_no_src});
                std::string text; tui::expand_row(text, value, at, edge.end);
                line.spans.push_back({std::move(text), style, offset + at});
                line.width = indent + edge.width; out.push_back(std::move(line));
                at = edge.next;
            } while (at < value.size());
        };
        const char kind = block.source.front();
        const int indent = result_indent();
        if (kind == '\x18') {
            const auto end = std::min(block.source.find('\n', 1), block.source.size());
            const auto tone = block.meta == "system.error" || block.meta == "system.stopped" ? theme.error
                : block.meta == "system.warning" ? theme.warning : theme.info;
            append(std::string_view(block.source).substr(1, end - 1), 1, 2,
                   block.meta == "system.error" ? theme.error : theme.text_muted,
                   std::string(icons().item) + " ", tone);
            if (end < block.source.size()) append(std::string_view(block.source).substr(end + 1), end + 1,
                indent, block.meta == "system.error" ? theme.error : theme.text_muted, result_prefix(), theme.text_faint);
        } else if (kind == '\x19') {
            for (size_t at = 1; at < block.source.size();) {
                const auto end = std::min(block.source.find('\n', at), block.source.size());
                const char state = block.source[at];
                const auto symbol = state == 'D' ? icons().done : icons().todo;
                const auto tone = state == 'D' ? theme.success : state == 'A' ? theme.accent : theme.text_muted;
                auto style = state == 'A' ? theme.text : theme.text_muted;
                if (state == 'A') style.attrs = style.attrs | tui::Attr::bold;
                if (state == 'X') style.attrs = style.attrs | tui::Attr::strike;
                const size_t first = out.size();
                append(std::string_view(block.source).substr(at + 1, end - at - 1), at + 1,
                       indent + display_width(symbol) + 1, style,
                       (out.empty() ? result_prefix() : std::string(static_cast<size_t>(indent), ' ')) + std::string(symbol) + " ", tone);
                // 结果挂钩与复选框分别使用 faint / 状态色。
                auto& spans = out[first].spans;
                spans.front().text = std::string(symbol) + " ";
                spans.insert(spans.begin(), {first == 0 ? result_prefix() : std::string(static_cast<size_t>(indent), ' '), theme.text_faint, tui::k_no_src});
                at = end + 1;
            }
        } else {
            const auto options = std::min(block.source.find('\x1e', 1), block.source.size());
            auto question = std::string_view(block.source).substr(1, options - 1);
            if (question.ends_with('\n')) question.remove_suffix(1);
            append(question, 1, indent, theme.text_muted, result_prefix(), theme.text_faint);
            const int room = std::max(1, width - indent);
            int used = room;
            for (size_t at = options; at < block.source.size();) {
                const auto end = std::min(block.source.find('\n', at), block.source.size());
                const bool chosen = block.source[at + 1] == 'S';
                const auto label = std::string_view(block.source).substr(at + 2, end - at - 2);
                const std::string check = chosen ? std::string(icons().check) + " " : "";
                const int chip = 2 + display_width(check) + display_width(label);
                if (used + 1 + chip > room) used = room;
                size_t pos = 0;
                do {
                    if (used == room) {
                        tui::Line line; line.offset = at + 2 + pos;
                        line.spans.push_back({std::string(static_cast<size_t>(indent), ' '), theme.text_faint, tui::k_no_src});
                        line.width = indent; out.push_back(std::move(line)); used = 0;
                    }
                    auto& line = out.back();
                    if (used) { line.spans.push_back({" ", theme.text_muted, tui::k_no_src}); ++used; ++line.width; }
                    auto style = theme.text_muted;
                    style.bg = chosen ? theme.background_selected.bg : theme.background_element.bg;
                    const auto prefix = pos == 0 ? check : std::string{};
                    const int budget = std::max(1, room - used - 2 - display_width(prefix));
                    const auto edge = tui::wrap_next_row(label, pos, budget);
                    line.spans.push_back({" ", style, tui::k_no_src});
                    if (!prefix.empty()) { auto tick = theme.success; tick.bg = style.bg; line.spans.push_back({prefix, tick, tui::k_no_src}); }
                    std::string text; tui::expand_row(text, label, pos, edge.end);
                    line.spans.push_back({std::move(text), style, at + 2 + pos});
                    line.spans.push_back({" ", style, tui::k_no_src});
                    const int added = 2 + display_width(prefix) + edge.width;
                    used += added; line.width += added; pos = edge.next;
                    if (pos < label.size()) used = room;
                } while (pos < label.size());
                at = end + 1;
            }
        }
        if (block.collapsed && out.size() > block.collapsed_rows) out.resize(block.collapsed_rows);
        return out.size();
    }

    size_t render_tool(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                       std::vector<tui::Line>& out, bool nested = false) const {
        out.clear(); out.emplace_back();
        auto& line = out.front(); line.offset = 0;
        if (block.meta == "tool.fold") {
            const auto& body = *doc_.find(std::stoull(block.source.substr(1)));
            if (body.row_count <= body.collapsed_rows) { out.clear(); return 0; }
            const std::string value = body.collapsed
                ? format_text(body.collapsed_rows == 0 ? ui::text().card_lines : ui::text().card_folded,
                              body.row_count - body.collapsed_rows, icons().separator)
                : std::string(ui::text().card_collapse);
            const auto prefix = body.collapsed && body.collapsed_rows == 0 ? result_prefix()
                : std::string(static_cast<size_t>(result_indent()), ' ') + (icons().item == "*" ? "... " : "… ");
            line.spans.push_back({prefix, theme.text_faint, tui::k_no_src});
            line.spans.push_back({fit(value, width - display_width(prefix)), theme.text_faint, tui::k_no_src});
            line.width = std::min(width, display_width(prefix) + display_width(value));
            return 1;
        }
        const std::size_t begin = block.source.starts_with('\x1e') ? 1 : 0;
        const std::size_t first_tab = block.source.find('\t', begin);
        const std::size_t second_tab = first_tab == std::string::npos
            ? std::string::npos : block.source.find('\t', first_tab + 1);
        const std::string name = block.source.substr(begin, first_tab - begin);
        const std::string param = first_tab == std::string::npos ? ""
            : block.source.substr(first_tab + 1, second_tab - first_tab - 1);
        const auto third_tab = second_tab == std::string::npos ? std::string::npos
            : block.source.find('\t', second_tab + 1);
        std::string stat = second_tab == std::string::npos ? ""
            : block.source.substr(second_tab + 1, third_tab - second_tab - 1);
        const std::string task = third_tab == std::string::npos ? "" : block.source.substr(third_tab + 1);
        std::string symbol = std::string(icons().spinner[frame_]) + " "; tui::Style symbol_style = theme.accent;
        if (block.meta == "tool.done") { symbol = std::string(icons().item) + " "; symbol_style = theme.success; }
        else if (block.meta == "tool.error") { symbol = std::string(icons().item) + " "; symbol_style = theme.error; }
        else if (block.meta == "tool.stopped") { symbol = std::string(icons().item) + " "; symbol_style = theme.error; }
        line.spans.push_back({std::string(symbol), symbol_style, tui::k_no_src});
        tui::Style heading = theme.text; heading.attrs = heading.attrs | tui::Attr::bold;
        const auto shown_name = fit_columns(name, std::max(0, width - display_width(symbol)));
        line.spans.push_back({shown_name, heading, begin});
        int used = display_width(symbol) + display_width(shown_name);
        const int param_room = param.empty() ? 0 : std::min(16, std::max(0, width - used) / 2);
        stat = fit_columns(stat, std::max(0, width - used - 2 - param_room));
        const int stat_width = display_width(stat);
        const int room = std::max(0, width - used - stat_width - (stat.empty() ? 0 : 2));
        if (!param.empty() && room > 2) {
            const std::string shown = fit_columns(param, room - 2);
            line.spans.push_back({"  ", theme.text_muted, tui::k_no_src});
            line.spans.push_back({shown, third_tab == std::string::npos ? theme.text_muted : theme.text, first_tab + 1});
            used += 2 + display_width(shown);
        }
        const int task_room = width - used - stat_width - (stat.empty() ? 0 : 2) - 2;
        if (!task.empty() && task_room > 0) {
            const auto shown = fit_columns(task, task_room);
            line.spans.push_back({"  ", theme.text_muted, tui::k_no_src});
            line.spans.push_back({shown, theme.text_muted, third_tab + 1});
            used += 2 + display_width(shown);
        }
        if (!stat.empty() && used + 2 + stat_width <= width) {
            line.spans.push_back({std::string(static_cast<std::size_t>(nested ? 2 : width - used - stat_width), ' '),
                                  theme.text_muted, tui::k_no_src});
            if (stat.starts_with('+') && stat.find(' ') != std::string::npos) {
                const auto split = stat.find(' ');
                auto added = theme.success, removed = theme.error;
                added.attrs = added.attrs & ~tui::Attr::bold;
                removed.attrs = removed.attrs & ~tui::Attr::bold;
                line.spans.push_back({stat.substr(0, split), added, tui::k_no_src});
                line.spans.push_back({" ", theme.text_muted, tui::k_no_src});
                line.spans.push_back({std::string(icons().minus) + stat.substr(split + 2), removed, tui::k_no_src});
            } else line.spans.push_back({stat, nested ? theme.text_faint : theme.text_muted, tui::k_no_src});
            used = nested ? used + 2 + stat_width : width;
        }
        line.width = used;
        return 1;
    }

    size_t render_banner(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                         size_t from, size_t valid, std::vector<tui::Line>& out) const {
        std::string source = block.source;
        if (width < 60 || icons().bar == "|") {
            const std::size_t marker = source.find("BANNER_END\n");
            if (marker != std::string::npos) source.erase(0, marker + 11);
        } else {
            const std::size_t marker = source.find("BANNER_END\n");
            if (marker != std::string::npos) source.erase(marker, 11);
        }
        tui::Block copy = block; copy.source = std::move(source);
        const size_t count = tui::TextRenderer(&tui::ThemeTokens::text_muted).render(
            copy, std::max(1, width - 2), theme, from, valid, out);
        for (size_t i = valid; i < count; ++i) {
            if (i < 6 && width >= 60 && icons().bar != "|") for (auto& span : out[i].spans) span.style = theme.primary;
            out[i].spans.insert(out[i].spans.begin(), {"  ", theme.text_muted, tui::k_no_src});
            out[i].width += 2;
        }
        return count;
    }
};

class DiffBodyRenderer final : public tui::BlockRenderer {
    struct Row {
        std::string_view code;
        size_t offset;
        int number = 0;
        char sign = ' ';
        bool hunk = false;
        int old_number = 0;
    };
    struct Diff {
        std::vector<Row> rows;
        int digits = 1, side_digits = 1;
    };
    static Diff parse(std::string_view source) {
        Diff diff;
        int old_line = 0, new_line = 0, largest = 0, largest_old = 0, hunks = 0;
        bool starts_at_one = false;
        for (size_t at = 0; at < source.size();) {
            const auto end = std::min(source.find('\n', at), source.size());
            auto line = source.substr(at, end - at);
            if (line.ends_with('\r')) line.remove_suffix(1);
            if (line.starts_with("@@ -")) {
                const auto plus = line.find(" +"), close = line.find(" @@", 4);
                if (plus != std::string_view::npos && close != std::string_view::npos) {
                    std::from_chars(line.data() + 4, line.data() + plus, old_line);
                    std::from_chars(line.data() + plus + 2, line.data() + close, new_line);
                    ++hunks;
                    starts_at_one = old_line <= 1 && new_line <= 1 && (old_line == 1 || new_line == 1);
                    auto context = line.substr(close + 3);
                    while (context.starts_with(' ')) context.remove_prefix(1);
                    diff.rows.push_back({context, at, 0, ' ', true});
                }
            } else if (hunks == 0 && (line.starts_with("--- ") || line.starts_with("+++ "))) {
                // 文件名已在卡片标题中显示。
            } else if (hunks > 0 && !line.empty() &&
                       (line.front() == '+' || line.front() == '-' || line.front() == ' ')) {
                const char sign = line.front();
                const int number = sign == '-' ? old_line : new_line;
                const int old_number = old_line;
                if (sign != '+') ++old_line;
                if (sign != '-') ++new_line;
                largest = std::max(largest, number);
                if (sign != '+') largest_old = std::max(largest_old, old_number);
                diff.rows.push_back({line.substr(1), at + 1, number, sign, false, old_number});
            } else {
                diff.rows.push_back({line, at});
            }
            at = end + 1;
        }
        if (hunks == 1 && starts_at_one)
            std::erase_if(diff.rows, [](const Row& row) { return row.hunk; });
        diff.digits = static_cast<int>(std::to_string(largest).size());
        diff.side_digits = static_cast<int>(std::to_string(std::max(largest, largest_old)).size());
        return diff;
    }
    struct Pair { const Row* left; const Row* right; };
    static std::vector<Pair> pair_rows(const Diff& diff) {
        std::vector<Pair> result;
        for (size_t i = 0; i < diff.rows.size();) {
            const auto& row = diff.rows[i];
            if (row.sign == '-' || row.sign == '+') {
                const auto begin = i;
                while (i < diff.rows.size() && diff.rows[i].sign == '-') ++i;
                const auto added = i;
                while (i < diff.rows.size() && diff.rows[i].sign == '+') ++i;
                for (size_t n = 0; n < std::max(added - begin, i - added); ++n)
                    result.push_back({n < added - begin ? &diff.rows[begin + n] : nullptr,
                                      n < i - added ? &diff.rows[added + n] : nullptr});
            } else { result.push_back({&row, &row}); ++i; }
        }
        return result;
    }
    size_t render_split(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                        const Diff& diff, std::vector<tui::Line>& out) const {
        const int left_width = (width - 1) / 2;
        const auto side = [&](tui::Line& line, const Row* row, bool right, int cols) {
            if (!row) { line.spans.push_back({std::string(static_cast<size_t>(cols), ' '), theme.text, tui::k_no_src}); return; }
            auto code = row->sign == '+' ? theme.diff_added : row->sign == '-' ? theme.diff_removed : theme.text;
            auto muted = theme.text_faint;
            if (row->sign != ' ') code.bg = diff_background(theme, row->sign == '+');
            const int number = right ? row->number : row->old_number;
            line.spans.push_back({number ? std::format("{:>{}} ", number, diff.side_digits)
                                        : std::string(static_cast<size_t>(diff.side_digits + 1), ' '), muted, tui::k_no_src});
            line.spans.push_back({row->sign == '-' ? std::string(icons().minus) + " " : row->sign == '+' ? "+ " : "  ", code, tui::k_no_src});
            const int room = cols - diff.side_digits - 3;
            std::string value;
            tui::expand_row(value, row->code, 0, row->code.size());
            const bool clipped = display_width(value) > room;
            if (clipped) value = fit(value, room - 1);
            const int used = display_width(value) + (clipped ? 1 : 0);
            line.spans.push_back({std::move(value), code, row->offset});
            if (clipped) line.spans.push_back({"…", code, tui::k_no_src});
            line.spans.push_back({std::string(static_cast<size_t>(room - used), ' '), code, tui::k_no_src});
        };
        for (const auto& pair : pair_rows(diff)) {
            tui::Line line;
            const auto* row = pair.left ? pair.left : pair.right;
            line.offset = row->offset; line.reordered = true;
            if (row->hunk) {
                line.spans.push_back({fit_columns("⋯" + (row->code.empty() ? std::string{} : " " + std::string(row->code)), width),
                                      theme.text_faint, tui::k_no_src});
            } else {
                side(line, pair.left, false, left_width);
                line.spans.push_back({std::string(icons().vertical), theme.text_faint, tui::k_no_src});
                side(line, pair.right, true, width - left_width - 1);
            }
            line.width = width;
            out.push_back(std::move(line));
        }
        if (block.collapsed && out.size() > block.collapsed_rows) out.resize(block.collapsed_rows);
        return out.size();
    }
public:
    static int available_width(int width) { return std::clamp(width - 4, 0, 176); }
    static int required_width(std::string_view source) { return result_indent() + 2 * (parse(source).side_digits + 63) + 1; }
    tui::WrapResult measure(std::string_view source, size_t from, int width) const override {
        if (from >= source.size()) return {};
        const auto diff = parse(source);
        if (width >= required_width(source)) return {pair_rows(diff).size(), 0, 0};
        size_t rows = 0;
        for (const auto& row : diff.rows)
            rows += row.hunk ? 1 : std::max(size_t{1}, tui::wrap_measure_from(
                row.code, 0, std::max(1, width - result_indent() - (row.number ? diff.digits + 3 : 0))).rows);
        return {rows, 0, 0};
    }
    size_t render(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                  size_t, size_t, std::vector<tui::Line>& out) const override {
        out.clear();
        const auto diff = parse(block.source);
        if (width >= required_width(block.source)) {
            render_split(block, width - result_indent(), theme, diff, out);
            for (auto& line : out) hang_result(line, &line == &out.front(), theme);
            return out.size();
        }
        for (const auto& row : diff.rows) {
            const int prefix = result_indent() + (row.number ? diff.digits + 3 : 0);
            const int room = std::max(1, width - prefix);
            size_t at = 0;
            do {
                tui::Line line;
                line.offset = row.offset + at;
                line.spans.push_back({out.empty() ? result_prefix() : std::string(static_cast<size_t>(result_indent()), ' '), theme.text_faint, tui::k_no_src});
                if (row.hunk) {
                    const auto value = std::string(icons().vertical == "|" ? "..." : "⋯") +
                        (row.code.empty() ? "" : " " + std::string(row.code));
                    const auto shown = fit_columns(value, std::max(0, width - result_indent()));
                    line.spans.push_back({shown, theme.text_faint, tui::k_no_src});
                    line.width = result_indent() + display_width(shown);
                    out.push_back(std::move(line));
                    break;
                }
                auto muted = theme.text_faint;
                auto code = row.sign == '+' ? theme.diff_added : row.sign == '-' ? theme.diff_removed : theme.text;
                const bool tinted = row.sign == '+' || row.sign == '-';
                if (tinted) code.bg = diff_background(theme, row.sign == '+');
                if (row.number) {
                    const auto number = at == 0 ? std::format("{:>{}}", row.number, diff.digits)
                                               : std::string(static_cast<size_t>(diff.digits), ' ');
                    line.spans.push_back({number + " ", muted, tui::k_no_src});
                    const auto sign = at == 0 && row.sign == '-' ? std::string(icons().minus)
                                                                 : std::string(1, at == 0 ? row.sign : ' ');
                    line.spans.push_back({sign + " ", code, tui::k_no_src});
                }
                const auto edge = tui::wrap_next_row(row.code, at, room);
                std::string value;
                tui::expand_row(value, row.code, at, edge.end);
                line.spans.push_back({std::move(value), code, row.offset + at});
                line.width = prefix + edge.width;
                if (tinted) {
                    line.spans.push_back({std::string(static_cast<size_t>(std::max(0, width - line.width)), ' '), code, tui::k_no_src});
                    line.width = width;
                }
                out.push_back(std::move(line));
                at = edge.next;
            } while (at < row.code.size());
        }
        if (block.collapsed && out.size() > block.collapsed_rows) out.resize(block.collapsed_rows);
        return out.size();
    }
};

class GutterRenderer final : public tui::BlockRenderer {
public:
    GutterRenderer(std::unique_ptr<tui::BlockRenderer> inner, const std::set<uint64_t>& starts)
        : inner_(std::move(inner)), starts_(starts) {}
    tui::WrapResult measure(std::string_view source, size_t from, int width) const override {
        return inner_->measure(source, from, std::max(1, width - 2));
    }
    size_t render(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                  size_t from, size_t valid, std::vector<tui::Line>& out) const override {
        const size_t count = inner_->render(block, std::max(1, width - 2), theme, from, valid, out);
        for (size_t i = valid; i < count; ++i) {
            out[i].spans.insert(out[i].spans.begin(),
                                {i == 0 && starts_.contains(block.id) ? std::string(icons().item) + " " : "  ", theme.text,
                                 tui::k_no_src});
            out[i].width += 2;
        }
        return count;
    }
private:
    std::unique_ptr<tui::BlockRenderer> inner_;
    const std::set<uint64_t>& starts_;
};

struct LinkRange {
    size_t begin, end;
    uint32_t id;
    bool underline;
};

uint32_t file_link(std::string_view name, const std::filesystem::path& project) {
    if (name.empty() || project.empty()) return 0;
    std::error_code ec;
    const auto path = std::filesystem::canonical(project / std::filesystem::path(name), ec);
    if (ec || !std::filesystem::is_regular_file(path, ec)) return 0;
    const auto relative = path.lexically_relative(project);
    if (relative.empty() || *relative.begin() == "..") return 0;
    std::string uri = "file://";
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char c : path.generic_string()) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') uri += static_cast<char>(c);
        else { uri += '%'; uri += hex[c >> 4]; uri += hex[c & 15]; }
    }
    return tui::intern_hyperlink(uri);
}

std::vector<LinkRange> find_links(const tui::Block& block, const std::filesystem::path& project) {
    const std::string_view source = block.source;
    std::vector<LinkRange> links;
    const auto add = [&](size_t begin, size_t end, uint32_t id, bool underline) {
        if (id && begin < end) links.push_back({begin, end, id, underline});
    };
    // 与 Markdown 渲染器的行内链接语法一致；工具原文也可链接其 label。
    for (size_t at = 0; (at = source.find('[', at)) != std::string_view::npos; ++at) {
        const auto close = source.find(']', at + 1);
        if (close == std::string_view::npos || close + 1 >= source.size() || source[close + 1] != '(') continue;
        const auto end = source.find(')', close + 2);
        if (end == std::string_view::npos) continue;
        auto target = source.substr(close + 2, end - close - 2);
        if (target.starts_with('<') && target.ends_with('>')) target = target.substr(1, target.size() - 2);
        auto id = tui::intern_hyperlink(target);
        if (!id) id = file_link(target, project);
        add(at + 1, close, id, true);
        at = end;
    }
    // 卡片参数可为含空格的完整文件名。
    if (block.meta.starts_with("tool.") && source.starts_with('\x1e')) {
        const auto begin = source.find('\t'), end = source.find('\t', begin + 1);
        if (begin != std::string_view::npos && end != std::string_view::npos)
            add(begin + 1, end, file_link(source.substr(begin + 1, end - begin - 1), project), false);
    }
    const auto delimiter = [](unsigned char c) {
        return c <= 0x20 || c == 0x7f || c == '`' || c == '"' || c == '\'' || c == '<' || c == '>' ||
               c == '[' || c == ']' || c == '(' || c == ')' || c == '{' || c == '}' || c == '=' || c == ',' || c == ';';
    };
    for (size_t at = 0; at < source.size();) {
        if (delimiter(static_cast<unsigned char>(source[at]))) { ++at; continue; }
        const size_t begin = at;
        const bool url = source.substr(at).starts_with("https://") || source.substr(at).starts_with("http://");
        if (url) {
            int parentheses = 0;
            while (at < source.size()) {
                const auto c = static_cast<unsigned char>(source[at]);
                if (c <= 0x20 || c == 0x7f || c == '`' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '[' || c == ']') break;
                if (c >= 0x80) {
                    auto rest = source.substr(at);
                    const auto cp = tui::unicode::decode_utf8(rest);
                    if ((cp >= 0x3000 && cp <= 0x303f) || (cp >= 0xff01 && cp <= 0xff65)) break;
                }
                if (c == '(') ++parentheses;
                if (c == ')' && --parentheses < 0) break;
                ++at;
            }
        } else while (at < source.size() && !delimiter(static_cast<unsigned char>(source[at]))) ++at;
        size_t end = at;
        while (end > begin && (source[end - 1] == '.' || source[end - 1] == ':' || source[end - 1] == '!' || source[end - 1] == '?' || source[end - 1] == ',' || source[end - 1] == ';')) --end;
        if (url) add(begin, end, tui::intern_hyperlink(source.substr(begin, end - begin)), true);
        else {
            const auto colon = source.substr(begin, end - begin).find(':');
            if (colon != std::string_view::npos) end = begin + colon;
            const auto name = source.substr(begin, end - begin);
            if (name.find('.') != std::string_view::npos || name.find('/') != std::string_view::npos)
                add(begin, end, file_link(name, project), false);
        }
    }
    std::stable_sort(links.begin(), links.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
    return links;
}

void link_line(tui::Line& line, std::string_view source, const std::vector<LinkRange>& links) {
    std::vector<tui::Span> spans;
    for (auto& span : line.spans) {
        if (span.src == tui::k_no_src) { spans.push_back(std::move(span)); continue; }
        size_t pos = span.src;
        int tab_left = 0;
        std::string_view remaining = span.text;
        tui::unicode::Grapheme g;
        while (tui::unicode::next_grapheme(remaining, g)) {
            auto style = span.style;
            for (const auto& link : links) {
                if (link.begin > pos) break;
                if (pos < link.end) {
                    style.hyperlink = link.id;
                    if (link.underline) style.attrs = style.attrs | tui::Attr::underline;
                    break;
                }
            }
            if (!spans.empty() && spans.back().style == style && spans.back().src != tui::k_no_src &&
                spans.back().src + spans.back().text.size() == pos) spans.back().text += g.bytes;
            else spans.push_back({std::string(g.bytes), style, pos});
            if (pos < source.size() && source[pos] == '\t') {
                if (!tab_left) {
                    const auto start = source.rfind('\n', pos);
                    auto prefix = source.substr(start == std::string_view::npos ? 0 : start + 1,
                        pos - (start == std::string_view::npos ? 0 : start + 1));
                    int col = 0;
                    tui::unicode::Grapheme before;
                    while (tui::unicode::next_grapheme(prefix, before))
                        col += before.bytes == "\t" ? 8 - col % 8 : before.width;
                    tab_left = 8 - col % 8;
                }
                if (!--tab_left) ++pos;
            } else if (pos < source.size()) {
                auto rest = source.substr(pos);
                tui::unicode::Grapheme original;
                if (tui::unicode::next_grapheme(rest, original)) pos += original.bytes.size();
            }
        }
    }
    line.spans = std::move(spans);
}

// 普通块仍使用原阅读栏；双栏 diff body 在主区两侧留白，限宽并同轴居中。
class TranscriptRenderer final : public tui::BlockRenderer {
public:
    TranscriptRenderer(std::unique_ptr<tui::BlockRenderer> inner, bool wide, bool links,
                       const std::filesystem::path& project)
        : inner_(std::move(inner)), wide_(wide), links_(links), project_(project) {}
    int columns(std::string_view source, int width) const {
        const int diff_width = DiffBodyRenderer::available_width(width);
        return wide_ && diff_width >= DiffBodyRenderer::required_width(source) ? diff_width : reading_width(width);
    }
    tui::WrapResult measure(std::string_view source, size_t from, int width) const override {
        auto result = inner_->measure(source, from, columns(source, width));
        // URL 可能跨增量片段增长；变化的块重新物化链接，其他块仍走缓存。
        if (links_) result.stable_rows = result.stable_bytes = 0;
        return result;
    }
    size_t render(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                  size_t from, size_t valid, std::vector<tui::Line>& out) const override {
        const int cols = columns(block.source, width);
        const auto count = inner_->render(block, cols, theme, from, valid, out);
        const auto links = links_ ? find_links(block, project_) : std::vector<LinkRange>{};
        for (size_t i = valid; i < count; ++i) {
            if (!links.empty()) link_line(out[i], block.source, links);
            const int inset = (width - cols) / 2;
            if (inset) out[i].spans.insert(out[i].spans.begin(),
                {std::string(static_cast<size_t>(inset), ' '), theme.background, tui::k_no_src});
            out[i].width += inset;
        }
        return count;
    }
private:
    std::unique_ptr<tui::BlockRenderer> inner_;
    bool wide_, links_;
    const std::filesystem::path& project_;
};

bool not_executed(std::string_view text) {
    constexpr std::string_view skipped[] = {kDenied, kDeniedWithFeedback, kPolicyDenied,
                                            kApprovalUnavailable, kPriorDenied, kToolLimit};
    return std::ranges::any_of(skipped, [&](std::string_view value) {
        return text.starts_with(value);
    });
}
tui::BlockKind body_kind(std::string_view name) {
    if (name == "edit" || name == "write") return tui::BlockKind::diff;
    return tui::BlockKind::text;
}
std::string title_name(std::string_view name) {
    if (name.empty()) return std::string(ui::text().card_tool);
    if (name == "todo") return std::string(ui::text().card_plan);
    if (name == "ask_user") return "Ask";
    std::string value(name);
    value.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(value.front())));
    return value;
}
} // namespace

Transcript::Transcript(tui::Document& doc, std::function<void(const TodoList&)> todo,
                       std::function<void(const FileChangeView&)> changed,
                       std::function<void(const std::string&)> user)
    : doc_(doc), on_changed_(std::move(changed)), on_user_(std::move(user)), on_todo_(std::move(todo)) {
    install_renderers();
}

void Transcript::install_renderers() {
    const auto install = [&](tui::BlockKind kind, std::unique_ptr<tui::BlockRenderer> renderer) {
        doc_.set_renderer(kind, std::make_unique<TranscriptRenderer>(std::move(renderer),
            kind == tui::BlockKind::diff && render_options_.auto_diff && !render_options_.unified,
            render_options_.links && kind != tui::BlockKind::output && kind != tui::BlockKind::diff,
            render_options_.project));
    };
    install(tui::BlockKind::text, std::make_unique<ChatRenderer>(frame_, doc_));
    install(tui::BlockKind::output, std::make_unique<UserRenderer>());
    install(tui::BlockKind::diff, std::make_unique<DiffBodyRenderer>());
    install(tui::BlockKind::code, std::make_unique<GutterRenderer>(std::make_unique<tui::SyntaxRenderer>(), assistant_starts_));
    install(tui::BlockKind::markdown, std::make_unique<GutterRenderer>(std::make_unique<tui::MarkdownRenderer>(), assistant_starts_));
    install(tui::BlockKind::table, std::make_unique<GutterRenderer>(std::make_unique<tui::TableRenderer>(), assistant_starts_));
}

void Transcript::set_render_options(RenderOptions options) {
    if (render_options_.auto_diff == options.auto_diff && render_options_.unified == options.unified &&
        render_options_.links == options.links && render_options_.project == options.project) return;
    render_options_.auto_diff = options.auto_diff;
    render_options_.unified = options.unified;
    render_options_.links = options.links;
    std::error_code ec;
    render_options_.project = std::filesystem::canonical(options.project, ec);
    if (ec) render_options_.project.clear();
    install_renderers();
}

bool Transcript::can_switch_diff(const tui::Scrollback& scroll, bool visible_only) const {
    if (!render_options_.auto_diff) return false;
    const int width = DiffBodyRenderer::available_width(scroll.rect().w);
    return std::ranges::any_of(tools_, [&](const auto& entry) {
        const auto& tool = entry.second;
        return tool.diff_width > 0 && width >= tool.diff_width && (!visible_only || scroll.block_visible(tool.body));
    });
}

void Transcript::refresh_icons() {
    for (const auto& [id, source] : icon_sources_) doc_.replace(id, source());
}

void Transcript::set_compact(bool value) {
    if (compact_ == value) return;
    compact_ = value;
    for (auto& [id, blocks] : tools_) if (blocks.task && blocks.finished) {
        refresh_task_body(blocks);
        collapse(blocks);
    }
    refresh_density();
    for (const auto& [id, margin] : margins_) {
        if (const auto* found = doc_.find(id)) {
            auto& block = const_cast<tui::Block&>(*found);
            block.margin_top = compact_ ? 0 : margin;
            doc_.replace(id, block.source);
        }
    }
}
void Transcript::refresh_density() {
    // Document owns mutable Blocks; notify it through replace after changing the public
    // layout metadata, so both row counts and materialized rows are invalidated.
    // An empty end marker bounds blocks added by MarkdownStream without changing its API.
    const auto end = doc_.append_block(tui::BlockKind::text);
    for (auto id = density_end_ + 1; id < end; ++id) {
        if (const auto* found = doc_.find(id)) {
            const bool assistant = found->kind == tui::BlockKind::markdown ||
                found->kind == tui::BlockKind::code || found->kind == tui::BlockKind::table;
            if (assistant && assistant_start_pending_) {
                assistant_starts_.insert(id); assistant_start_pending_ = false;
            }
            const uint8_t margin = found->meta.starts_with("user:") ? found->margin_top : 0;
            margins_[id] = margin;
            auto& block = const_cast<tui::Block&>(*found);
            block.margin_top = compact_ ? 0 : margin;
            doc_.replace(id, block.source);
        }
    }
    density_end_ = end;
    doc_.erase_from(end);
}

uint64_t Transcript::text(std::string source, std::string meta, bool open) {
    tui::Block block;
    if (meta.starts_with("system.")) source.insert(0, 1, '\x18');
    block.source = std::move(source); block.meta = std::move(meta);
    block.open = open;
    return doc_.append_block(std::move(block));
}
void Transcript::finish_message() {
    if (markdown_) { markdown_->finish(); refresh_density(); markdown_.reset(); }
    finish_thought();
}

// 无正文不留标题；流式与历史思考使用相同的展开状态。
void Transcript::finish_thought() {
    if (!thought_title_) return;
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - thought_began_).count();
    const auto* body = doc_.find(reasoning_);
    const bool empty = !body || body->source.substr(1).find_first_not_of(" \t\r\n") == std::string::npos;
    doc_.set_meta(thought_title_, "thought");
    doc_.replace(thought_title_, empty ? "" : format_text(ui::text().thought_closed, seconds));
    if (reasoning_) {
        if (empty) doc_.replace(reasoning_, {});
        doc_.close_block(reasoning_);
        doc_.set_collapsed(reasoning_, !thoughts_expanded_, 0);
        if (!empty) thoughts_.push_back({thought_title_, reasoning_, seconds});
    }
    thought_title_ = 0; reasoning_ = 0;
}

void Transcript::toggle_thoughts() {
    thoughts_expanded_ = !thoughts_expanded_;
    for (const Thought& thought : thoughts_) doc_.set_collapsed(thought.body, !thoughts_expanded_, 0);
    if (reasoning_) doc_.set_collapsed(reasoning_, !thoughts_expanded_, 0);
}

void Transcript::set_session(std::string mode, std::string model) {
    mode_ = std::move(mode); model_ = std::move(model);
}
Transcript::ToolBlocks& Transcript::tool(const std::string& id, const std::string& name,
                                         const std::string& summary) {
    auto [it, inserted] = tools_.try_emplace(id);
    if (inserted) {
        auto& blocks = it->second;
        blocks.group = next_group_++; blocks.began = std::chrono::steady_clock::now();
        blocks.task = name == "task";
        if (blocks.task) {
            const auto end = summary.find("): ");
            if (summary.starts_with("task(") && end != std::string::npos) {
                blocks.task_agent = clean_field(summary.substr(5, end - 5));
                blocks.task_description = clean_field(summary.substr(end + 3));
            } else blocks.task_description = clean_field(summary);
        }
        tui::Block title_block;
        if (name == "ask_user") {
            const std::string_view prefix = "Ask the user: ";
            blocks.ask_prompt = clean_field(summary.starts_with(prefix) ? summary.substr(prefix.size()) : summary);
        }
        title_block.source = "\x1e" + title_name(name) + "\t" +
            (name == "ask_user" ? "" : clean_field(summary)) + "\t";
        title_block.meta = "tool.running"; title_block.group = blocks.group;
        blocks.title = doc_.append_block(std::move(title_block));
        tui::Block body_block;
        body_block.kind = body_kind(name); body_block.open = true;
        if (body_block.kind == tui::BlockKind::text) body_block.source = "\x1d";
        body_block.meta = "tool.body"; body_block.group = blocks.group;
        blocks.body = doc_.append_block(std::move(body_block));
        if (body_kind(name) == tui::BlockKind::diff) blocks.diff_width = DiffBodyRenderer::required_width("");
        tui::Block fold_block;
        fold_block.meta = "tool.fold"; fold_block.group = blocks.group;
        blocks.fold = doc_.append_block(std::move(fold_block));
        if (blocks.task) refresh_task_title(blocks);
    }
    return it->second;
}

void Transcript::show_question(const QuestionRequest& question) {
    const auto prompt = clean_field(question.prompt);
    // 问答 DTO 不带 call_id，以当前未完成 Ask 的问题匹配对应卡片。
    for (auto& [id, blocks] : tools_) {
        if (blocks.finished || blocks.ask_prompt != prompt) continue;
        blocks.label = "\x1e" "Ask\t" + clean_field(question.header) + "\t";
        doc_.replace(blocks.title, blocks.label);
        doc_.replace(blocks.body, "\x1a" + question.prompt + "\n");
        break;
    }
}

void Transcript::finished(const ToolFinished& event) {
    finish_message();
    ToolBlocks& blocks = tool(event.id, event.name, event.summary);
    std::string name = title_name(event.name), param = clean_field(event.summary), stat, body;
    uint32_t rows = 3;
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - blocks.began).count();
    stat = format_text(ui::text().card_elapsed, elapsed);
    std::visit(base::Overloaded{
        [&](std::monostate) { body = event.text; },
        [&](const ReadView& v) {
            name = std::string(ui::text().card_read); param = v.path + (v.directory ? "/" : "");
            if (!v.directory) stat = format_text(ui::text().card_read_range, v.start_line, v.end_line, stat);
        },
        [&](const FileChangeView& v) {
            if (on_changed_ && !event.is_error && !event.interrupted) on_changed_(v);
            name = v.created ? std::string(ui::text().card_write) : std::string(ui::text().card_edit); param = v.path;
            stat = std::format("+{} -{}", v.added, v.removed); body = v.diff; rows = 20;
        },
        [&](const BashView& v) {
            name = std::string(ui::text().card_bash); param = clean_field(v.command);
            const std::string state = v.interrupted ? std::string(ui::text().card_interrupted) : v.timed_out ? std::string(ui::text().card_timeout)
                : format_text(ui::text().card_exit, v.exit_code.value_or(0));
            stat = format_text(ui::text().card_state_time, state, v.elapsed_ms / 1000.0);
            body = v.output; rows = 10;
        },
        [&](const GrepView& v) {
            name = std::string(ui::text().card_grep); param = "\"" + v.pattern + "\"";
            stat = format_text(v.lines.size() == 1 ? ui::text().card_match : ui::text().card_matches, v.lines.size(), stat);
            for (const auto& line : v.lines) body += std::format("{}:{}: {}\n", line.path, line.line, line.text);
            rows = 5;
        },
        [&](const GlobView& v) {
            name = std::string(ui::text().card_glob); param = v.pattern;
            stat = format_text(v.files.size() == 1 ? ui::text().card_file : ui::text().card_files, v.files.size(), stat);
            for (const auto& file : v.files) body += file + '\n';
            rows = 5;
        },
        [&](const WebView& v) {
            name = v.operation == "search" ? "Web search" : "Web fetch";
            param = clean_field(v.operation == "search" ? v.query : v.url);
            stat = std::format("HTTP {} · {:.1f}s{}{}", v.status, v.elapsed_ms / 1000.0,
                v.cached ? " · cached" : "", v.truncated ? " · truncated" : "");
            if (v.operation == "fetch") stat += std::format(" · {}–{}/{} bytes", v.offset, v.next_offset, v.total_bytes);
            body = v.output; rows = v.operation == "fetch" && !event.is_error ? 0 : 8;
        },
        [&](const McpView& v) {
            name = v.server + "." + v.tool; param.clear();
            for (const auto& content : v.content) {
                body += content.value("type", "") == "text" ? content.value("text", "")
                                                               : "[" + content.value("type", std::string(ui::text().card_content)) + "]";
                body += '\n';
            }
            rows = 5;
        },
        [&](const TodoList& v) {
            name = std::string(ui::text().card_plan);
            const auto done = std::ranges::count_if(v, [](const TodoItem& item) {
                return item.state == TodoItem::State::done;
            });
            param = std::format("{} / {}", done, v.size()); stat.clear();
            body = "\x19";
            for (const auto& item : v) {
                const char state = item.state == TodoItem::State::done ? 'D' : item.state == TodoItem::State::doing ? 'A'
                    : item.state == TodoItem::State::dropped ? 'X' : 'P';
                body += state + clean_field(item.text) + "\n";
            }
            rows = UINT32_MAX;
            if (on_todo_) on_todo_(v);
        },
        [&](const AskView& v) {
            name = "Ask"; param = clean_field(v.header);
            body = "\x1a" + v.prompt + "\n";
            for (std::size_t i = 0; i < v.options.size(); ++i) {
                const bool chosen = std::ranges::find(v.selected, static_cast<int>(i)) != v.selected.end();
                body += std::string("\x1e") + (chosen ? "S" : "P") + clean_field(v.options[i].label) + "\n";
            }
            if (!v.other.empty()) body += "\x1eSOther: " + clean_field(v.other) + "\n";
            if (v.cancelled) stat = "Cancelled";
            rows = UINT32_MAX;
        },
        [&](const MemoryView& v) {
            name = "memory"; param = v.op + " " + v.scope + "/" + v.name;
            body = v.type + " " + v.updated + "\n" + v.description + "\n\n" + v.content;
            rows = 8;
        },
        [&](const SkillView& v) {
            name = std::string(ui::text().card_skill); param = v.name;
            body = v.path + "\n" + event.text;
        },
        [&](const TaskView& v) {
            name = std::string(ui::text().card_task);
            blocks.task = true;
            blocks.task_agent = param = clean_field(v.agent);
            if (!v.task.empty()) blocks.task_description = clean_field(v.task);
            blocks.task_result = trim_blank_lines(v.result);
            stat = task_stats(v.tool_calls, v.seconds);
            for (auto& [id, child] : blocks.children) {
                if (child[1] == 'R') child[1] = 'E';
            }
            if (blocks.children.empty()) for (const TaskStep& step : v.steps)
                blocks.children.emplace_back("", std::string("\x1e") + (step.is_error ? "E" : "S") +
                                                 clean_field(step.summary) + "\t\t");
            rows = 10;
            tasks_.push_back({v.agent, v.session_id, event.id, event.is_error || event.interrupted, v.seconds, blocks.task_description});
        }
    }, event.view);
    if (body.empty() && event.is_error) body = event.text;
    if (blocks.task && !std::holds_alternative<TaskView>(event.view))
        blocks.task_result = trim_blank_lines(body);
    const bool skipped = std::holds_alternative<std::monostate>(event.view) && not_executed(event.text);
    blocks.label = "\x1e" + name + "\t" + param + "\t" + stat;
    if (blocks.task) blocks.label += "\t" + blocks.task_description;
    doc_.set_meta(blocks.title, event.interrupted || skipped ? "tool.stopped"
                               : event.is_error ? "tool.error" : "tool.done");
    doc_.replace(blocks.title, blocks.label);
    if (blocks.diff_width && !std::holds_alternative<FileChangeView>(event.view)) {
        auto& block = const_cast<tui::Block&>(*doc_.find(blocks.body));
        block.kind = tui::BlockKind::text; blocks.diff_width = 0;
    }
    if (blocks.task) refresh_task_body(blocks);
    else {
        const bool structured = std::holds_alternative<AskView>(event.view) || std::holds_alternative<TodoList>(event.view);
        if (!blocks.diff_width && !structured) {
            // 只删完全重复参数的首行，保留真正的结果及错误细节。
            const auto end = std::min(body.find('\n'), body.size());
            const auto first = std::string_view(body).substr(0, end);
            const auto label = first.find(": ");
            if (!param.empty() && (first == param ||
                (label != std::string_view::npos && first.substr(label + 2) == param)))
                body.erase(0, std::min(end + 1, body.size()));
            body.insert(0, 1, '\x1d');
        }
        doc_.replace(blocks.body, base::to_valid_utf8(body));
    }
    if (event.is_error || event.interrupted) doc_.set_meta(blocks.body, "tool.body.error");
    doc_.close_block(blocks.body);
    if (blocks.diff_width) blocks.diff_width = DiffBodyRenderer::required_width(doc_.find(blocks.body)->source);
    blocks.rows = rows; blocks.finished = true;
    collapse(blocks);
}

void Transcript::tick() {
    frame_ = (frame_ + 1) % icons().spinner.size();
    for (auto& [id, blocks] : tools_) if (!blocks.finished) {
        if (blocks.task) refresh_task_title(blocks);
        else if (const auto* block = doc_.find(blocks.title)) doc_.replace(blocks.title, block->source);
        if (!blocks.children.empty()) if (const auto* block = doc_.find(blocks.body)) doc_.replace(blocks.body, block->source);
    }
}
void Transcript::refresh_task_title(ToolBlocks& blocks) {
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - blocks.began).count();
    blocks.label = "\x1e" + std::string(ui::text().card_task) + "\t" + blocks.task_agent + "\t" +
        task_stats(blocks.children.size(), seconds) + "\t" + blocks.task_description;
    doc_.replace(blocks.title, blocks.label);
}
void Transcript::refresh_task_body(ToolBlocks& blocks) {
    std::string body = "\x1c";
    for (const auto& [id, child] : blocks.children) body += child + "\n";
    if (!blocks.task_result.empty()) {
        if (!blocks.children.empty() && !compact_) body += "\n";
        body += blocks.task_result;
    }
    doc_.replace(blocks.body, base::to_valid_utf8(body));
}
void Transcript::apply_child(const std::string& call_id, const Event& event, const Transcript& child_transcript) {
    const auto parent = tools_.find(call_id);
    if (parent == tools_.end() || parent->second.finished) return;
    auto& blocks = parent->second;
    if (const auto* started = std::get_if<TurnStarted>(&event.payload)) {
        blocks.task_description = clean_field(started->input);
        refresh_task_title(blocks);
        return;
    }
    std::string id, row;
    if (const auto* started = std::get_if<ToolStarted>(&event.payload)) {
        id = started->id;
        row = "\x1eR" + title_name(started->name) + "\t" + clean_field(started->summary) + "\t";
    } else if (const auto* done = std::get_if<ToolFinished>(&event.payload)) {
        id = done->id;
        // 子 Pane 已投影完同一事件，直接复用其名称、参数和真实统计。
        row = child_transcript.tools_.at(id).label;
        const auto& title = *child_transcript.doc_.find(child_transcript.tools_.at(id).title);
        row.insert(1, title.meta == "tool.done" ? "S" : "E");
    } else return;
    auto child = std::ranges::find_if(blocks.children, [&](const auto& value) { return value.first == id; });
    if (child == blocks.children.end()) blocks.children.emplace_back(id, row);
    else child->second = std::move(row);
    refresh_task_body(blocks);
    refresh_task_title(blocks);
}

void Transcript::apply_live(const Event& event) {
    apply(event);
    if (const auto* ended = std::get_if<TurnEnded>(&event.payload)) {
        finish_message();
        for (auto& [id, blocks] : tools_) if (!blocks.finished) {
            doc_.set_meta(blocks.title, "tool.stopped");
            doc_.set_meta(blocks.body, "tool.body.error");
            doc_.close_block(blocks.body); blocks.finished = true;
            if (!blocks.children.empty()) {
                std::string body = "\x1c";
                for (auto& [child_id, row] : blocks.children) {
                    if (row[1] == 'R') row[1] = 'E';
                    body += row + "\n";
                }
                doc_.replace(blocks.body, std::move(body));
            }
        }
        switch (ended->status) {
        case TurnStatus::interrupted: {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - turn_began_).count();
            auto source = [seconds] { return std::string(icons().cross) + " " + std::string(ui::text().card_turn_interrupted)
                + std::string(icons().separator) + format_text(ui::text().card_elapsed, seconds); };
            const auto id = text(source(), "msg.footer.error");
            icon_sources_[id] = std::move(source);
            break;
        }
        case TurnStatus::denied: text(std::string(ui::text().card_denied), "system.stopped"); break;
        case TurnStatus::limit: text(std::string(ui::text().card_limit), "system.stopped"); break;
        case TurnStatus::failed: text(ended->error, "system.error"); break;
        case TurnStatus::done:
            if (live_step_ && !model_.empty()) {
                const double seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - turn_began_).count();
                auto footer = format_text(ui::text().turn_footer, mode_, model_, seconds, icons().separator);
                if (ended->output) footer += std::string(icons().separator) + format_text(ui::text().turn_output, compact_tokens(*ended->output));
                text(std::move(footer), "msg.footer");
            }
            break;
        }
    }
    refresh_density();
}

void Transcript::append_history(const protocol::HistoryItem& item) {
    for (const Event& event : decode_history(item)) apply(event);
    if (item.kind == "assistant") finish_message();
    refresh_density();
}

void Transcript::apply(const Event& event) {
    std::visit(base::Overloaded{
        [&](const TurnStarted& e) {
            if (on_user_) on_user_(e.input);
            finish_message(); live_step_ = false; turn_began_ = std::chrono::steady_clock::now();
            tui::Block block;
            block.kind = tui::BlockKind::output; block.meta = "user:" + e.time;
            block.source = e.input; block.margin_top = has_user_ ? 1 : 0; has_user_ = true;
            doc_.append_block(std::move(block));
        },
        [&](const StepStarted&) {
            finish_message(); live_step_ = true;
            step_start_ = doc_.append_block(tui::BlockKind::text);
            markdown_ = std::make_unique<tui::MarkdownStream>(doc_); assistant_start_pending_ = true;
        },
        [&](const TextDelta& e) {
            if (!e.text.empty()) finish_thought();
            if (!markdown_) { markdown_ = std::make_unique<tui::MarkdownStream>(doc_); assistant_start_pending_ = true; }
            markdown_->feed(e.text);
        },
        [&](const ReasoningDelta& e) {
            if (e.text.empty()) return;
            if (!reasoning_) {
                if (e.text.find_first_not_of(" \t\r\n") == std::string::npos) return;
                thought_began_ = std::chrono::steady_clock::now();
                thought_title_ = text(std::string(ui::text().thought_live), "thought.live");
                tui::Block body;
                body.meta = "thought.body"; body.open = true; body.source = "\x1b";
                body.collapsed = !thoughts_expanded_; body.collapsed_rows = 0;
                reasoning_ = doc_.append_block(std::move(body));
            }
            doc_.append(reasoning_, e.text);
        },
        [&](const StreamReset&) {
            markdown_.reset(); reasoning_ = 0; thought_title_ = 0;
            if (step_start_) doc_.erase_from(step_start_);
            step_start_ = doc_.append_block(tui::BlockKind::text);
            markdown_ = std::make_unique<tui::MarkdownStream>(doc_); assistant_start_pending_ = true;
        },
        [&](const ToolStarted& e) { finish_message(); tool(e.id, e.name, e.summary); },
        [&](const ToolOutput& e) {
            if (auto it = tools_.find(e.id); it != tools_.end()) {
                doc_.append(it->second.body, e.chunk);
                if (it->second.diff_width)
                    it->second.diff_width = DiffBodyRenderer::required_width(doc_.find(it->second.body)->source);
            }
        },
        [&](const ToolFinished& e) { finished(e); },
        [&](const Compacted& e) {
            text(format_text(ui::text().card_compacted, e.before, e.after), "system.compact");
        },
        [&](const ModelChanged& e) { model_ = e.model; },
        [&](const ModeChanged& e) { mode_ = e.planning ? "plan" : e.mode; },
        [&](const Notice& e) {
            if (e.level == NoticeLevel::error) text(e.text, "system.error");
            else if (e.persistent) text(e.text, e.level == NoticeLevel::warn ? "system.warning" : "system.status");
        },
        [](const auto&) {}
    }, event.payload);
}

void Transcript::clear() {
    margins_.clear(); icon_sources_.clear(); assistant_starts_.clear();
    has_user_ = false; assistant_start_pending_ = true; density_end_ = 0;
    markdown_.reset(); reasoning_ = step_start_ = thought_title_ = 0;
    thoughts_.clear(); tools_.clear(); tasks_.clear(); doc_.clear();
    next_group_ = 1;
    if (on_todo_) on_todo_({});
}
void Transcript::info(std::string value) { text(std::move(value), "system.status"); refresh_density(); }
void Transcript::banner(std::string version, std::string cwd, std::string branch,
                        int mcp_servers) {
    static constexpr std::string_view logo =
        "██████╗  █████╗  ██████╗ ███████╗███╗   ██╗████████╗\n"
        "██╔══██╗██╔══██╗██╔════╝ ██╔════╝████╗  ██║   ██║\n"
        "██║  ██║███████║██║  ██╗ █████╗  ██╔██╗ ██║   ██║\n"
        "██║  ██║██╔══██║██║  ╚██╗██╔══╝  ██║╚██╗██║   ██║\n"
        "██████╔╝██║  ██║╚██████╔╝███████╗██║ ╚████║   ██║\n"
        "╚═════╝ ╚═╝  ╚═╝ ╚═════╝ ╚══════╝╚═╝  ╚═══╝   ╚═╝\n"
        "BANNER_END\n";
    auto source = [version = std::move(version), cwd = std::move(cwd), branch = std::move(branch), mcp_servers] {
        std::string value(logo);
        value += format_text(ui::text().banner_identity, version, cwd,
                             branch.empty() ? "" : "   " + std::string(icons().branch) + " " + branch, mcp_servers);
        value += std::string(ui::text().banner_hint);
        return value;
    };
    const auto id = text(source(), "banner");
    icon_sources_[id] = std::move(source);
    refresh_density();
}
void Transcript::resumed(std::string id, std::size_t messages, std::string age) {
    text(format_text(ui::text().banner_resumed, id.substr(0, 8), messages, age), "system.status");
}
void Transcript::toggle_tools() {
    expanded_ = !expanded_;
    for (auto& [id, blocks] : tools_) if (blocks.finished) collapse(blocks);
}
void Transcript::collapse(ToolBlocks& blocks) {
    doc_.set_collapsed(blocks.body, !expanded_, blocks.rows);
    doc_.replace(blocks.fold, std::string(1, '\x1f') + std::to_string(blocks.body));
}

} // namespace dagent::ui
