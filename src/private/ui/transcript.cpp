#include "ui/transcript.hpp"
#include "ui/strings.hpp"

#include <algorithm>
#include <cctype>
#include <format>

#include "agent/conversation.hpp"
#include "base/text.hpp"
#include "tui/grapheme.hpp"

namespace dagent::ui {
namespace {
template<class... T> struct Overloaded : T... { using T::operator()...; };

int display_width(std::string_view value) {
    int width = 0;
    tui::unicode::Grapheme g;
    while (tui::unicode::next_grapheme(value, g)) width += g.width;
    return width;
}

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

std::string ellipsize(std::string_view value, int columns) {
    if (display_width(value) <= columns) return std::string(value);
    return columns <= 3 ? fit("...", columns) : fit(value, columns - 3) + "...";
}

std::string clean_field(std::string value) {
    std::ranges::replace(value, '\t', ' ');
    std::ranges::replace(value, '\n', ' ');
    return value;
}

class ChatRenderer final : public tui::BlockRenderer {
public:
    tui::WrapResult measure(std::string_view source, size_t from, int width) const override {
        if (source.empty()) return {};
        if (const std::size_t marker = source.find("BANNER_END\n"); marker != std::string_view::npos) {
            std::string visible(source);
            if (width < 60) visible.erase(0, marker + 11);
            else visible.erase(marker, 11);
            return tui::wrap_measure_from(visible, from, std::max(1, width - 2));
        }
        if (source.starts_with('\x1e') || source.starts_with('\x1f')) return {1, 1, source.size()};
        return tui::wrap_measure_from(source, from, std::max(1, width - (width < 60 ? 1 : 2)));
    }

    size_t render(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                  size_t from, size_t valid, std::vector<tui::Line>& out) const override {
        if (block.source.empty()) { out.clear(); return 0; }
        if (block.meta.starts_with("tool.body"))
            return render_body(block, width, theme, from, valid, out);
        if (block.meta.starts_with("tool.")) return render_tool(block, width, theme, out);
        if (block.meta == "banner") return render_banner(block, width, theme, from, valid, out);

        tui::Style style = theme.text;
        std::string first = "  ", next = "  ";
        if (block.meta == "user") first = next = width < 60 ? "▌" : "▌ ";
        else if (block.meta == "thought") style = theme.accent;
        else if (block.meta == "msg.footer") style = theme.text_muted;
        else if (block.meta == "thought.body") { // 与 Thought 标题同列，不再加符号
            style = theme.text_muted; style.attrs = style.attrs | tui::Attr::italic;
        } else if (block.meta == "system.error") style = theme.error;
        else if (block.meta.starts_with("system.")) style = theme.text_muted;
        else if (block.meta == "todo") first = next = width < 60 ? "▌" : "▌ ";

        const int gutter = display_width(first);
        const size_t count = tui::TextRenderer(&tui::ThemeTokens::text).render(
            block, std::max(1, width - gutter), theme, from, valid, out);
        for (size_t i = valid; i < count; ++i) {
            auto& line = out[i];
            for (auto& span : line.spans) span.style = style;
            if (block.meta == "user") {
                for (auto& span : line.spans) span.style.bg = theme.background_element.bg;
                const int fill_columns = std::max(0, width - line.width - gutter);
                line.spans.push_back({std::string(static_cast<std::size_t>(fill_columns), ' '),
                                      theme.background_element, tui::k_no_src});
            }
            const std::string& prefix = i == 0 ? first : next;
            const tui::Style prefix_style = block.meta == "user" || block.meta == "todo"
                                                ? theme.primary : style;
            line.spans.insert(line.spans.begin(), {prefix, prefix_style, tui::k_no_src});
            line.width += gutter;
        }
        return count;
    }

private:
    size_t render_body(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                       size_t from, size_t valid, std::vector<tui::Line>& out) const {
        const size_t count = tui::TextRenderer().render(block, std::max(1, width - 2),
                                                        theme, from, valid, out);
        for (size_t i = valid; i < count; ++i) {
            out[i].spans.insert(out[i].spans.begin(), {"│ ", theme.border, tui::k_no_src});
            out[i].width += 2;
        }
        return count;
    }

    size_t render_tool(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                       std::vector<tui::Line>& out) const {
        out.clear(); out.emplace_back();
        auto& line = out.front(); line.offset = 0;
        if (block.meta == "tool.fold") {
            const std::string_view value = block.source.starts_with('\x1f')
                ? std::string_view(block.source).substr(1) : std::string_view(block.source);
            line.spans.push_back({"└ ", theme.text_muted, tui::k_no_src});
            line.spans.push_back({fit(value, width - 2), theme.text_muted, 1});
            line.width = std::min(width, 2 + display_width(value));
            return 1;
        }
        const std::size_t begin = block.source.starts_with('\x1e') ? 1 : 0;
        const std::size_t first_tab = block.source.find('\t', begin);
        const std::size_t second_tab = first_tab == std::string::npos
            ? std::string::npos : block.source.find('\t', first_tab + 1);
        const std::string name = block.source.substr(begin, first_tab - begin);
        const std::string param = first_tab == std::string::npos ? ""
            : block.source.substr(first_tab + 1, second_tab - first_tab - 1);
        const std::string stat = second_tab == std::string::npos ? "" : block.source.substr(second_tab + 1);
        std::string_view symbol = "● "; tui::Style symbol_style = theme.accent;
        if (block.meta == "tool.done") { symbol = "✓ "; symbol_style = theme.success; }
        else if (block.meta == "tool.error") { symbol = "✗ "; symbol_style = theme.error; }
        else if (block.meta == "tool.stopped") { symbol = "◌ "; symbol_style = theme.text_muted; }
        line.spans.push_back({std::string(symbol), symbol_style, tui::k_no_src});
        tui::Style heading = theme.text; heading.attrs = heading.attrs | tui::Attr::bold;
        line.spans.push_back({name, heading, begin});
        int used = display_width(symbol) + display_width(name);
        const int stat_width = display_width(stat);
        const int room = std::max(0, width - used - stat_width - (stat.empty() ? 0 : 2));
        if (!param.empty() && room > 2) {
            const std::string shown = ellipsize(param, room - 2);
            line.spans.push_back({"  ", theme.text_muted, tui::k_no_src});
            line.spans.push_back({shown, theme.text_muted, first_tab + 1});
            used += 2 + display_width(shown);
        }
        if (!stat.empty() && used + 2 + stat_width <= width) {
            line.spans.push_back({std::string(static_cast<std::size_t>(width - used - stat_width), ' '),
                                  theme.text_muted, tui::k_no_src});
            line.spans.push_back({stat, theme.text_muted, tui::k_no_src});
            used = width;
        }
        line.width = used;
        return 1;
    }

    size_t render_banner(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                         size_t from, size_t valid, std::vector<tui::Line>& out) const {
        std::string source = block.source;
        if (width < 60) {
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
            if (i < 6 && width >= 60) for (auto& span : out[i].spans) span.style = theme.primary;
            out[i].spans.insert(out[i].spans.begin(), {"  ", theme.text_muted, tui::k_no_src});
            out[i].width += 2;
        }
        return count;
    }
};

class GutterRenderer final : public tui::BlockRenderer {
public:
    explicit GutterRenderer(std::unique_ptr<tui::BlockRenderer> inner) : inner_(std::move(inner)) {}
    tui::WrapResult measure(std::string_view source, size_t from, int width) const override {
        return inner_->measure(source, from, std::max(1, width - 2));
    }
    size_t render(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                  size_t from, size_t valid, std::vector<tui::Line>& out) const override {
        const bool tool = block.meta.starts_with("tool.body");
        const size_t count = inner_->render(block, std::max(1, width - 2), theme, from, valid, out);
        for (size_t i = valid; i < count; ++i) {
            out[i].spans.insert(out[i].spans.begin(),
                                {tool ? "│ " : "  ", tool ? theme.border : theme.text,
                                 tui::k_no_src});
            out[i].width += 2;
        }
        return count;
    }
private:
    std::unique_ptr<tui::BlockRenderer> inner_;
};

bool not_executed(std::string_view text) {
    constexpr std::string_view skipped[] = {agent::texts::kDenied, agent::texts::kDeniedWithFeedback,
                                            agent::texts::kPolicyDenied, agent::texts::kPriorDenied,
                                            agent::texts::kToolLimit};
    return std::ranges::any_of(skipped, [&](std::string_view value) {
        return text.starts_with(value.substr(0, value.find("{}")));
    });
}
uint32_t line_count(std::string_view text) {
    return static_cast<uint32_t>(std::ranges::count(text, '\n')) +
           (!text.empty() && !text.ends_with('\n'));
}
tui::BlockKind body_kind(std::string_view name) {
    if (name == "bash") return tui::BlockKind::output;
    if (name == "edit" || name == "write") return tui::BlockKind::diff;
    if (name == "grep" || name == "glob") return tui::BlockKind::code;
    return tui::BlockKind::text;
}
std::string title_name(std::string_view name) {
    if (name.empty()) return std::string(ui::text().card_tool);
    if (name == "todo") return std::string(ui::text().card_plan);
    std::string value(name);
    value.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(value.front())));
    return value;
}
} // namespace

Transcript::Transcript(tui::Document& doc, std::function<void(const tools::TodoView&)> todo)
    : doc_(doc), on_todo_(std::move(todo)) {
    doc_.set_renderer(tui::BlockKind::text, std::make_unique<ChatRenderer>());
    doc_.set_renderer(tui::BlockKind::output,
                      std::make_unique<GutterRenderer>(std::make_unique<tui::TextRenderer>()));
    doc_.set_renderer(tui::BlockKind::diff,
                      std::make_unique<GutterRenderer>(std::make_unique<tui::DiffRenderer>()));
    doc_.set_renderer(tui::BlockKind::code,
                      std::make_unique<GutterRenderer>(std::make_unique<tui::SyntaxRenderer>()));
    doc_.set_renderer(tui::BlockKind::markdown,
                      std::make_unique<GutterRenderer>(std::make_unique<tui::MarkdownRenderer>()));
    doc_.set_renderer(tui::BlockKind::table,
                      std::make_unique<GutterRenderer>(std::make_unique<tui::TableRenderer>()));
}

uint64_t Transcript::text(std::string source, std::string meta, bool open) {
    tui::Block block;
    block.source = std::move(source); block.meta = std::move(meta);
    block.margin_top = 1; block.open = open;
    return doc_.append_block(std::move(block));
}
void Transcript::finish_message() {
    if (markdown_) { markdown_->finish(); markdown_.reset(); }
    finish_thought();
    has_text_ = false;
}

// 思考定稿：标题换成 +/- Thought: Ns，正文按当前展开状态收起。
void Transcript::finish_thought() {
    if (!thought_title_) return;
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - thought_began_).count();
    doc_.replace(thought_title_, format_text(thoughts_expanded_ ? ui::text().thought_open
                                                                : ui::text().thought_closed, seconds));
    if (reasoning_) {
        doc_.close_block(reasoning_);
        doc_.set_collapsed(reasoning_, !thoughts_expanded_, 0);
        thoughts_.push_back({thought_title_, reasoning_, seconds});
    }
    thought_title_ = 0; reasoning_ = 0;
}

void Transcript::toggle_thoughts() {
    thoughts_expanded_ = !thoughts_expanded_;
    for (const Thought& thought : thoughts_) {
        doc_.replace(thought.title, format_text(thoughts_expanded_ ? ui::text().thought_open
                                                                   : ui::text().thought_closed,
                                                thought.seconds));
        doc_.set_collapsed(thought.body, !thoughts_expanded_, 0);
    }
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
        tui::Block title_block;
        title_block.source = "\x1e" + title_name(name) + "\t" + clean_field(summary) + "\t";
        title_block.meta = "tool.running"; title_block.margin_top = 1; title_block.group = blocks.group;
        blocks.title = doc_.append_block(std::move(title_block));
        tui::Block body_block;
        body_block.kind = body_kind(name); body_block.open = true;
        body_block.meta = "tool.body"; body_block.group = blocks.group;
        blocks.body = doc_.append_block(std::move(body_block));
        tui::Block fold_block;
        fold_block.meta = "tool.fold"; fold_block.group = blocks.group;
        blocks.fold = doc_.append_block(std::move(fold_block));
    }
    return it->second;
}

void Transcript::finished(const agent::ToolFinished& event) {
    finish_message();
    ToolBlocks& blocks = tool(event.id, event.name, event.summary);
    std::string name = title_name(event.name), param = clean_field(event.summary), stat, body;
    uint32_t rows = 3;
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - blocks.began).count();
    stat = format_text(ui::text().card_elapsed, elapsed);
    std::visit(Overloaded{
        [&](std::monostate) { body = event.result.text; },
        [&](const tools::ReadView& v) {
            name = std::string(ui::text().card_read); param = v.path + (v.directory ? "/" : "");
            if (!v.directory) stat = format_text(ui::text().card_read_range, v.start_line, v.end_line, stat);
        },
        [&](const tools::FileChangeView& v) {
            name = v.created ? std::string(ui::text().card_write) : std::string(ui::text().card_edit); param = v.path;
            stat = format_text(ui::text().card_changes, v.added, v.removed, stat); body = v.diff; rows = 20;
        },
        [&](const tools::BashView& v) {
            name = std::string(ui::text().card_bash); param = clean_field(v.command);
            const std::string state = v.interrupted ? std::string(ui::text().card_interrupted) : v.timed_out ? std::string(ui::text().card_timeout)
                : v.signal ? format_text(ui::text().card_signal, *v.signal)
                           : format_text(ui::text().card_exit, v.exit_code.value_or(0));
            stat = format_text(ui::text().card_state_time, state, v.elapsed_ms / 1000.0);
            body = v.output; rows = 10;
        },
        [&](const tools::GrepView& v) {
            name = std::string(ui::text().card_grep); param = "\"" + v.pattern + "\"";
            stat = format_text(ui::text().card_matches, v.lines.size(), stat);
            for (const auto& line : v.lines) body += std::format("{}:{}: {}\n", line.path, line.line, line.text);
            rows = 5;
        },
        [&](const tools::GlobView& v) {
            name = std::string(ui::text().card_glob); param = v.pattern; stat = format_text(ui::text().card_files, v.files.size(), stat);
            for (const auto& file : v.files) body += file + '\n';
            rows = 5;
        },
        [&](const tools::McpView& v) {
            name = v.server + "." + v.tool; param.clear();
            for (const auto& content : v.content) {
                body += content.value("type", "") == "text" ? content.value("text", "")
                                                               : "[" + content.value("type", std::string(ui::text().card_content)) + "]";
                body += '\n';
            }
            rows = 5;
        },
        [&](const tools::TodoView& v) {
            name = std::string(ui::text().card_plan); param = format_text(ui::text().card_items, v.items.size());
            const auto done = std::ranges::count_if(v.items, [](const tools::TodoItem& item) {
                return item.state == tools::TodoItem::State::done;
            });
            stat = format_text(ui::text().card_done, done, stat);
            todo_ = v; if (on_todo_) on_todo_(v); update_todo_block();
            const bool complete = !v.items.empty() && done == static_cast<std::ptrdiff_t>(v.items.size());
            if (complete && !todo_complete_) {
                std::string summary = format_text(ui::text().card_plan_complete, done, v.items.size());
                for (const auto& item : v.items) summary += " · " + item.text;
                text(std::move(summary), "system.todo");
            }
            todo_complete_ = complete;
        }
    }, event.result.display);
    if (body.empty() && event.result.is_error) body = event.result.text;
    const bool skipped = std::holds_alternative<std::monostate>(event.result.display) &&
                         not_executed(event.result.text);
    blocks.label = "\x1e" + name + "\t" + param + "\t" + stat;
    doc_.set_meta(blocks.title, event.result.interrupted || skipped ? "tool.stopped"
                               : event.result.is_error ? "tool.error" : "tool.done");
    doc_.replace(blocks.title, blocks.label);
    doc_.replace(blocks.body, base::to_valid_utf8(body)); doc_.close_block(blocks.body);
    blocks.rows = rows; blocks.finished = true; blocks.foldable = line_count(body) > rows;
    collapse(blocks);
}

void Transcript::apply(const agent::Event& event) {
    std::visit(Overloaded{
        [&](const agent::TurnStarted& e) {
            finish_message(); live_step_ = false; turn_began_ = std::chrono::steady_clock::now();
            text(e.input, "user");
        },
        [&](const agent::StepStarted&) {
            finish_message(); live_step_ = true;
            step_start_ = doc_.append_block(tui::BlockKind::text);
            markdown_ = std::make_unique<tui::MarkdownStream>(doc_, 1);
        },
        [&](const agent::TextDelta& e) {
            if (!e.text.empty()) finish_thought();
            has_text_ = has_text_ || !e.text.empty();
            if (!markdown_) markdown_ = std::make_unique<tui::MarkdownStream>(doc_, 1);
            markdown_->feed(e.text);
        },
        [&](const agent::ReasoningDelta& e) {
            if (!reasoning_) {
                thought_began_ = std::chrono::steady_clock::now();
                thought_title_ = text(std::string(ui::text().thought_live), "thought");
                tui::Block body;
                body.meta = "thought.body"; body.open = true;
                reasoning_ = doc_.append_block(std::move(body));
            }
            doc_.append(reasoning_, e.text);
        },
        [&](const agent::StreamReset&) {
            markdown_.reset(); reasoning_ = 0; thought_title_ = 0; has_text_ = false;
            if (step_start_) doc_.erase_from(step_start_);
            step_start_ = doc_.append_block(tui::BlockKind::text);
            markdown_ = std::make_unique<tui::MarkdownStream>(doc_, 1);
        },
        [&](const agent::ToolStarted& e) { finish_message(); tool(e.id, e.name, e.summary); },
        [&](const agent::ToolOutput& e) {
            if (auto it = tools_.find(e.id); it != tools_.end()) doc_.append(it->second.body, e.chunk);
        },
        [&](const agent::ToolFinished& e) { finished(e); },
        [&](const agent::Compacted& e) {
            text(format_text(ui::text().card_compacted, e.before, e.after), "system.compact");
        },
        [&](const agent::Notice& e) { if (e.level == agent::Notice::Level::error) text("✗ " + e.text, "system.error"); },
        [&](const agent::TurnEnded& e) {
            if (live_step_ && reasoning_ && e.status == agent::TurnStatus::interrupted) doc_.replace(reasoning_, {});
            if (live_step_ && has_text_ && markdown_ && e.status == agent::TurnStatus::interrupted)
                markdown_->feed(agent::texts::kInterrupted);
            finish_message();
            switch (e.status) {
            case agent::TurnStatus::interrupted: text(std::string(ui::text().card_turn_interrupted), "system.status"); break;
            case agent::TurnStatus::denied: text(std::string(ui::text().card_denied), "system.status"); break;
            case agent::TurnStatus::limit: text(std::string(ui::text().card_limit), "system.status"); break;
            case agent::TurnStatus::failed: text("✗ " + e.error, "system.error"); break;
            case agent::TurnStatus::done:
                if (live_step_ && !model_.empty()) {
                    const double seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - turn_began_).count();
                    text(format_text(ui::text().turn_footer, mode_, model_, seconds), "msg.footer");
                }
                break;
            }
        },
        [](const auto&) {}
    }, event);
}

void Transcript::clear() {
    markdown_.reset(); reasoning_ = step_start_ = todo_block_ = thought_title_ = 0;
    thoughts_.clear(); tools_.clear(); doc_.clear();
    todo_.items.clear(); todo_complete_ = false; next_group_ = 1;
    if (on_todo_) on_todo_(todo_);
}
void Transcript::info(std::string value) { text(std::move(value), "system.status"); }
void Transcript::banner(std::string version, std::string cwd, std::string branch,
                        std::string model, int mcp_servers) {
    static constexpr std::string_view logo =
        "██████╗  █████╗  ██████╗ ███████╗███╗   ██╗████████╗\n"
        "██╔══██╗██╔══██╗██╔════╝ ██╔════╝████╗  ██║   ██║\n"
        "██║  ██║███████║██║  ██╗ █████╗  ██╔██╗ ██║   ██║\n"
        "██║  ██║██╔══██║██║  ╚██╗██╔══╝  ██║╚██╗██║   ██║\n"
        "██████╔╝██║  ██║╚██████╔╝███████╗██║ ╚████║   ██║\n"
        "╚═════╝ ╚═╝  ╚═╝ ╚═════╝ ╚══════╝╚═╝  ╚═══╝   ╚═╝\n"
        "BANNER_END\n";
    std::string value(logo);
    value += format_text(ui::text().banner_identity, version, cwd,
                         branch.empty() ? "" : "   ⎇ " + branch, model, mcp_servers);
    value += std::string(ui::text().banner_hint);
    text(std::move(value), "banner");
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
    if (blocks.foldable) doc_.replace(blocks.fold, std::string(1, '\x1f') +
        (expanded_ ? std::string(ui::text().card_collapse) : format_text(ui::text().card_folded, blocks.rows)));
    else doc_.replace(blocks.fold, {});
}
void Transcript::set_todo_narrow(bool value) {
    if (todo_narrow_ == value) return;
    todo_narrow_ = value;
    update_todo_block();
}
void Transcript::set_todo_collapsed(bool value) {
    if (todo_collapsed_ == value) return;
    todo_collapsed_ = value;
    update_todo_block();
}
void Transcript::update_todo_block() {
    if (todo_.items.empty() && !todo_block_) return;
    std::string value;
    const auto done = std::ranges::count_if(todo_.items, [](const tools::TodoItem& item) {
        return item.state == tools::TodoItem::State::done;
    });
    if (todo_narrow_ && !todo_.items.empty()) {
        value = format_text(ui::text().status_plan, done, todo_.items.size());
        if (todo_collapsed_) value += std::string(ui::text().todo_expand);
        else for (const auto& item : todo_.items) {
            const std::string_view symbol = item.state == tools::TodoItem::State::done ? "✓"
                : item.state == tools::TodoItem::State::doing ? "●"
                : item.state == tools::TodoItem::State::dropped ? "✗" : "○";
            value += "\n" + std::string(symbol) + " " + item.text;
        }
    }
    if (!todo_block_) todo_block_ = text(std::move(value), "todo");
    else doc_.replace(todo_block_, std::move(value));
}

} // namespace dagent::ui
