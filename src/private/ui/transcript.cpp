#include "ui/transcript.hpp"

#include <algorithm>
#include <format>

#include "base/text.hpp"
#include "agent/conversation.hpp"

namespace dagent::ui {
namespace {
template<class... T> struct Overloaded : T... { using T::operator()...; };

// 两列装饰不进入源文本；保留 Span::src，拖选得到的仍是原始内容。
class ChatRenderer final : public tui::BlockRenderer {
public:
    tui::WrapResult measure(std::string_view source, size_t from, int width) const override {
        return tui::wrap_measure_from(source, from, std::max(1, width - 2));
    }
    size_t render(const tui::Block& block, int width, const tui::ThemeTokens& theme,
                  size_t from, size_t valid, std::vector<tui::Line>& out) const override {
        tui::Style tui::ThemeTokens::*style = &tui::ThemeTokens::text_muted;
        std::string prefix = "  ";
        if (block.meta == "user") { style = &tui::ThemeTokens::primary; prefix = "› "; }
        else if (block.meta == "reasoning") prefix = "∴ ";
        else if (block.meta == "tool.running") { style = &tui::ThemeTokens::accent; prefix = "● "; }
        else if (block.meta == "tool.done") { style = &tui::ThemeTokens::success; prefix = "✓ "; }
        else if (block.meta == "tool.stopped") prefix = "◌ ";
        else if (block.meta == "tool.error") { style = &tui::ThemeTokens::error; prefix = "✗ "; }
        else if (block.meta == "error") style = &tui::ThemeTokens::error;
        else if (block.meta == "warn") style = &tui::ThemeTokens::warning;
        const size_t count = tui::TextRenderer(style).render(block, std::max(1, width - 2),
                                                            theme, from, valid, out);
        for (size_t i = valid; i < count; ++i) {
            out[i].spans.insert(out[i].spans.begin(), {i == 0 ? prefix : "  ", theme.*style, tui::k_no_src});
            out[i].width += 2;
        }
        return count;
    }
};

// 没有执行的调用（拒绝、同批被拒、达到上限）只能从标准文本认出；参数化文本比较 {} 之前的固定部分。
bool not_executed(std::string_view text) {
    constexpr std::string_view skipped[] = {agent::texts::kDenied, agent::texts::kDeniedWithFeedback,
                                            agent::texts::kPolicyDenied, agent::texts::kPriorDenied,
                                            agent::texts::kToolLimit};
    return std::ranges::any_of(skipped, [&](std::string_view t) {
        return text.starts_with(t.substr(0, t.find("{}")));
    });
}

uint32_t line_count(std::string_view text) {
    const auto newlines = static_cast<uint32_t>(std::ranges::count(text, '\n'));
    return newlines + (!text.empty() && !text.ends_with('\n'));
}

tui::BlockKind body_kind(std::string_view name) {
    if (name == "bash") return tui::BlockKind::output;
    if (name == "edit" || name == "write") return tui::BlockKind::diff;
    if (name == "grep" || name == "glob") return tui::BlockKind::code;
    return tui::BlockKind::text;
}
} // namespace

Transcript::Transcript(tui::Document& doc) : doc_(doc) {
    doc_.set_renderer(tui::BlockKind::text, std::make_unique<ChatRenderer>());
}

uint64_t Transcript::text(std::string source, std::string meta, bool open) {
    tui::Block block;
    block.source = std::move(source);
    block.meta = std::move(meta);
    block.margin_top = 1;
    block.open = open;
    return doc_.append_block(std::move(block));
}

void Transcript::finish_message() {
    if (markdown_) { markdown_->finish(); markdown_.reset(); }
    if (reasoning_) { doc_.close_block(reasoning_); reasoning_ = 0; }
    has_text_ = false;
}

Transcript::ToolBlocks& Transcript::tool(const std::string& id, const std::string& name,
                                         const std::string& summary) {
    auto [it, inserted] = tools_.try_emplace(id);
    if (inserted) {
        it->second.title = text(summary, "tool.running");
        it->second.body = doc_.open_block(body_kind(name));
    }
    return it->second;
}

void Transcript::finished(const agent::ToolFinished& event) {
    finish_message();
    ToolBlocks& blocks = tool(event.id, event.name, event.summary);
    std::string title = event.summary, body;
    uint32_t rows = 3;
    std::visit(Overloaded{
        [&](std::monostate) { body = event.result.text; },
        [&](const tools::ReadView& v) {
            title = v.directory ? "读取目录 " + v.path
                                : std::format("读取 {} {}–{} 行", v.path, v.start_line, v.end_line);
        },
        [&](const tools::FileChangeView& v) {
            title = v.created ? std::format("创建 {}（{} 行）", v.path, v.added)
                              : std::format("编辑 {}（+{} −{}）", v.path, v.added, v.removed);
            body = v.diff; rows = 20;
        },
        [&](const tools::BashView& v) {
            title = "$ " + v.command;
            if (v.interrupted) title += "（已中断）";
            else if (v.timed_out) title += "（超时）";
            else if (v.signal) title += std::format("（信号 {}）", *v.signal);
            else title += std::format("（退出码 {} · {:.1f}s）", v.exit_code.value_or(0), v.elapsed_ms / 1000.0);
            if (v.sandbox != "read_only") title += " · 可写";
            if (v.allow_network) title += " · 联网";
            body = v.output; rows = 10;
        },
        [&](const tools::GrepView& v) {
            title = std::format("搜索 \"{}\"：{} 处", v.pattern, v.lines.size());
            for (const auto& line : v.lines) body += std::format("{}:{}: {}\n", line.path, line.line, line.text);
            rows = 5;
        },
        [&](const tools::GlobView& v) {
            title = std::format("查找 {}：{} 个文件", v.pattern, v.files.size());
            for (const auto& file : v.files) body += file + '\n';
            rows = 5;
        },
        [&](const tools::McpView& v) {
            title = v.server + "." + v.tool;
            for (const auto& c : v.content) {
                body += c.value("type", "") == "text" ? c.value("text", "") : "[" + c.value("type", "内容") + "]";
                body += '\n';
            }
            rows = 5;
        }
    }, event.result.display);
    if (body.empty() && event.result.is_error) body = event.result.text;
    const bool skipped = std::holds_alternative<std::monostate>(event.result.display) &&
                         not_executed(event.result.text);
    blocks.label = base::to_valid_utf8(title);
    doc_.set_meta(blocks.title, event.result.interrupted || skipped ? "tool.stopped"
                               : event.result.is_error ? "tool.error" : "tool.done");
    doc_.replace(blocks.body, base::to_valid_utf8(body));
    doc_.close_block(blocks.body);
    blocks.rows = rows;
    blocks.finished = true;
    blocks.foldable = line_count(body) > rows;
    collapse(blocks);
}

void Transcript::apply(const agent::Event& event) {
    std::visit(Overloaded{
        [&](const agent::TurnStarted& e) { finish_message(); live_step_ = false; text(e.input, "user"); },
        [&](const agent::StepStarted&) {
            finish_message();
            live_step_ = true;
            step_start_ = doc_.append_block(tui::BlockKind::text);
            markdown_ = std::make_unique<tui::MarkdownStream>(doc_, 1);
        },
        [&](const agent::TextDelta& e) {
            has_text_ = has_text_ || !e.text.empty();
            if (!markdown_) markdown_ = std::make_unique<tui::MarkdownStream>(doc_, 1);
            markdown_->feed(e.text);
        },
        [&](const agent::ReasoningDelta& e) {
            if (!reasoning_) {
                reasoning_ = text({}, "reasoning", true);
                doc_.set_collapsed(reasoning_, true, 3);
            }
            doc_.append(reasoning_, e.text);
        },
        [&](const agent::StreamReset&) {
            markdown_.reset(); reasoning_ = 0; has_text_ = false;
            if (step_start_) doc_.erase_from(step_start_);
            step_start_ = doc_.append_block(tui::BlockKind::text);
            markdown_ = std::make_unique<tui::MarkdownStream>(doc_, 1);
        },
        [&](const agent::ToolStarted& e) {
            finish_message(); tool(e.id, e.name, e.summary);
        },
        [&](const agent::ToolOutput& e) {
            if (auto it = tools_.find(e.id); it != tools_.end()) doc_.append(it->second.body, e.chunk);
        },
        [&](const agent::ToolFinished& e) { finished(e); },
        [&](const agent::Compacted& e) { info(std::format("上下文已压缩：{} → {} tokens", e.before, e.after)); },
        [&](const agent::Notice& e) { if (e.level == agent::Notice::Level::error) text(e.text, "error"); },
        [&](const agent::TurnEnded& e) {
            // 核心在取消时只保存正文 + T1，丢弃本步未完成的 reasoning。
            if (live_step_ && reasoning_ && e.status == agent::TurnStatus::interrupted) {
                doc_.replace(reasoning_, {});
            }
            if (live_step_ && has_text_ && markdown_ && e.status == agent::TurnStatus::interrupted) {
                markdown_->feed(agent::texts::kInterrupted);
            }
            finish_message();
            switch (e.status) {
            case agent::TurnStatus::interrupted: info("已中断"); break;
            case agent::TurnStatus::denied: info("已拒绝，等待你的指示"); break;
            case agent::TurnStatus::limit: info("达到本轮上限"); break;
            case agent::TurnStatus::failed: text(e.error, "error"); break;
            case agent::TurnStatus::done: break;
            }
        },
        [](const auto&) {}
    }, event);
}

void Transcript::clear() {
    markdown_.reset(); reasoning_ = step_start_ = 0; tools_.clear(); doc_.clear();
}
void Transcript::info(std::string value) { text(std::move(value), "info"); }
void Transcript::toggle_tools() {
    expanded_ = !expanded_;
    for (auto& [id, blocks] : tools_) {
        if (blocks.finished) collapse(blocks);
    }
}
void Transcript::collapse(ToolBlocks& blocks) {
    doc_.set_collapsed(blocks.body, !expanded_, blocks.rows);
    doc_.replace(blocks.title, blocks.label +
        (blocks.foldable ? (expanded_ ? " · Ctrl+O 折叠" : " · 已折叠，Ctrl+O 展开") : ""));
}
} // namespace dagent::ui
