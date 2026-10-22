#include "ui/page/transcript.hpp"
#include "ui/style/display.hpp"
#include "ui/style/strings.hpp"
#include "ui/style/appearance.hpp"

#include <algorithm>
#include <cctype>
#include <format>

#include "base/overloaded.hpp"
#include "base/text.hpp"
#include "tui/grid/grapheme.hpp"

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
    const int width = diff_available_width(scroll.rect().w);
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
        if (body_kind(name) == tui::BlockKind::diff) blocks.diff_width = diff_required_width("");
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
    if (blocks.diff_width) blocks.diff_width = diff_required_width(doc_.find(blocks.body)->source);
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
                    it->second.diff_width = diff_required_width(doc_.find(it->second.body)->source);
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
