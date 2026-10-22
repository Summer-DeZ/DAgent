#include "ui/page/transcript.hpp"
#include "ui/style/display.hpp"
#include "ui/style/strings.hpp"
#include "ui/style/appearance.hpp"
#include "ui/page/center.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

#include "base/overloaded.hpp"
#include "base/text.hpp"
#include "tui/grid/grapheme.hpp"

namespace dagent::ui {
namespace {
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

} // namespace

int diff_required_width(std::string_view source) { return DiffBodyRenderer::required_width(source); }
int diff_available_width(int width) { return DiffBodyRenderer::available_width(width); }

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

} // namespace dagent::ui
