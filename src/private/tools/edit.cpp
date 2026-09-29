#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/text.hpp"
#include "tools/detail.hpp"
#include "workspace/diff.hpp"

namespace dagent::tools {
using detail::count_lines;
using detail::error_result;
using detail::find_all;
using detail::fit_line;
using detail::get_bool;
using detail::line_at;
using detail::line_prefix;
using detail::require_string;
using detail::resolve_arg;
using detail::split_lines;

namespace {

constexpr std::string_view kDescription = R"(Replace an exact span of text in a file. Prefer this tool for changes to existing files.

- old_string must match the file exactly, without read output's line number prefixes, and must occur exactly once. Multiple matches return an error: expand the span to make it unique, or use replace_all=true.
- You must read the file before editing it. If someone changes it after your read, this tool reports it as stale; read it again.
- The original newline style and BOM are preserved.
- Success returns change statistics and four context lines around each change. Consecutive edits do not require a fresh read each time.)";

/// old_string 的每一行是不是都带着 read 输出的「行号 + Tab」前缀。
bool looks_like_numbered(std::string_view text) {
    bool any = false;
    for (const std::string_view line : split_lines(text)) {
        any = true;
        std::size_t i = 0;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        const std::size_t digits_begin = i;
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
        if (i == digits_begin || i >= line.size() || line[i] != '\t') return false;
    }
    return any;
}

std::string_view strip_indent(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    return line.substr(i);
}

/// 忽略每行行首缩进后做匹配，判断缩进不一致是不是失败的原因；命中时返回原文件里的行号。
std::optional<std::size_t> unique_indent_insensitive(std::string_view content, std::string_view old_lf) {
    std::string stripped_content;
    for (const std::string_view line : split_lines(content)) {
        stripped_content += strip_indent(line);
        stripped_content += '\n';
    }
    std::string stripped_old;
    for (const std::string_view line : split_lines(old_lf)) {
        stripped_old += strip_indent(line);
        stripped_old += '\n';
    }
    if (!stripped_old.empty() && stripped_old.back() == '\n' && !old_lf.ends_with('\n'))
        stripped_old.pop_back(); // old 末行不带换行时，去掉补的那一个
    const auto hits = find_all(stripped_content, stripped_old);
    if (hits.size() != 1) return std::nullopt;
    return line_at(stripped_content, hits[0]);
}

/// 改动区域：new 内容里的闭区间行号。
struct Region {
    std::size_t first = 0, last = 0;
};

/// 在 content 上执行替换；匹配数为 0 或（不 replace_all 时）多于 1 返回 nullopt。
/// 改动处前后各 4 行（带行号），多处改动时总量按预算截断。返回放不下的区域数。
std::size_t build_snippets(const std::string& new_content, const std::vector<Region>& regions,
                           std::size_t budget, std::size_t max_line_bytes, std::string& out) {
    const auto lines = split_lines(new_content);
    const std::size_t total = count_lines(new_content);
    std::vector<std::string> blocks;
    for (const Region& region : regions) {
        std::string block;
        const std::size_t from = region.first > 4 ? region.first - 4 : 1;
        const std::size_t to = std::min(total, region.last + 4);
        for (std::size_t i = from; i <= to; ++i)
            block += line_prefix(i) + fit_line(lines[i - 1], max_line_bytes) + "\n";
        blocks.push_back(std::move(block));
    }
    std::size_t dropped = 0;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        std::size_t need = blocks[i].size();
        if (i + 1 < blocks.size()) need += 3; // 分隔行 "--\n"
        if (out.size() + need > budget) {
            ++dropped;
            continue;
        }
        out += blocks[i];
        if (i + 1 < blocks.size()) out += "--\n";
    }
    return dropped;
}

class EditCall final : public PreparedTool {
public:
    EditCall(Context& ctx, workspace::Resolved target, std::string display, std::string old_string,
             std::string new_string, bool replace_all)
        : ctx_(ctx), target_(std::move(target)), reference_(target_.path), path_(std::move(display)),
          old_string_(std::move(old_string)), new_string_(std::move(new_string)), replace_all_(replace_all) {
        intent_.kind = agent::ToolKind::write;
        intent_.paths = {to_intent(target_, agent::Access::write)};
        intent_.summary = "Edit " + path_;
        expect_ = ctx_.tracked_stamp(target_);
        if (!expect_ || expect_ != reference_.stamp())
            throw workspace::WorkspaceError(workspace::WorkspaceError::Kind::stale,
                                            "read the current file before editing it");
    }
    std::optional<agent::PreparedIntent> preview_request() const override {
        agent::PreparedIntent request;
        request.kind = agent::ToolKind::read;
        request.paths = {to_intent(target_, agent::Access::read)};
        request.summary = "Read " + path_ + " for edit preview";
        return request;
    }
private:
    std::optional<Result> do_prepare_preview(const Grant& grant) override;
    Result do_execute(const Grant& grant, const std::function<void(std::string_view)>&, std::stop_token) override {
        ctx_.require_access(grant, target_, agent::Access::write);
        if (!ready_) return error_result("edit preview has not been authorized");
        ctx_.track(target_, reference_.write(new_content_, eol_, bom_, expect_, ctx_.files()));
        Result result;
        result.model_text = success_text_;
        result.display = view_;
        return result;
    }
    Context& ctx_;
    workspace::Resolved target_;
    workspace::FileReference reference_;
    std::string path_, old_string_, new_string_, new_content_, success_text_;
    bool replace_all_ = false, ready_ = false, bom_ = false;
    workspace::Eol eol_ = workspace::Eol::lf;
    std::optional<workspace::Stamp> expect_;
    agent::FileChangeView view_;
};

class EditTool final : public Tool {
public:
    EditTool() {
        spec_.name = "edit";
        spec_.description = std::string(kDescription);
        spec_.parameters = {
            {"type", "object"},
            {"properties",
             {{"path", {{"type", "string"}, {"description", "File path to edit, relative to the workspace root"}}},
              {"old_string", {{"type", "string"}, {"description", "Original text to replace; must match file contents exactly"}}},
              {"new_string", {{"type", "string"}, {"description", "Replacement text"}}},
              {"replace_all",
               {{"type", "boolean"}, {"description", "Replace all occurrences; by default old_string must match exactly once, otherwise an error is returned"}}}}},
            {"required", std::vector<std::string>{"path", "old_string", "new_string"}},
        };
    }

    const Spec& spec() const override { return spec_; }

    std::expected<std::unique_ptr<PreparedTool>, Result> prepare(
        std::string_view arguments, Context& ctx) const override {
        auto args = detail::parse_arguments(arguments);
        if (!args) return std::unexpected(error_result(args.error()));
        std::string err;
        const std::string path = require_string(*args, "path", err);
        const auto old_string = detail::get_string(*args, "old_string", err);
        const auto new_string = detail::get_string(*args, "new_string", err);
        const auto replace_all = get_bool(*args, "replace_all", err);
        if (!err.empty()) return std::unexpected(error_result(err));
        if (!old_string) return std::unexpected(error_result("old_string is required"));
        if (!new_string) return std::unexpected(error_result("new_string is required"));

        const workspace::Resolved resolved = resolve_arg(ctx, path);
        const std::string display = detail::display_path(ctx, resolved);

        return std::make_unique<EditCall>(ctx, resolved, display, detail::to_lf(*old_string),
                                           detail::to_lf(*new_string), replace_all.value_or(false));
    }

private:
    Spec spec_;
};

std::optional<Result> EditCall::do_prepare_preview(const Grant& grant) {
    ctx_.require_access(grant, target_, agent::Access::read);
    const auto file = reference_.read(ctx_.files());
    if (file.stamp != *expect_) return error_result("file changed since read; read it again");
    auto& ctx = ctx_;
    const auto& display = path_;
        if (file.lossy)
            return error_result(std::format(
                "{} contains invalid UTF-8 (replaced with U+FFFD when read); editing is denied to avoid corrupting the original file", display));
        if (file.truncated)
            return error_result(std::format(
                "{} is larger than the per-read limit; only its beginning was read. Editing is denied to avoid losing the remaining content", display));

        const std::string old_lf = old_string_;
        const std::string new_lf = new_string_;
        if (old_lf.empty()) return error_result("old_string must not be empty");
        if (old_lf == new_lf) return error_result("old_string and new_string are identical");

        const std::string& content = file.content;
        const std::vector<std::size_t> positions = find_all(content, old_lf);

        if (positions.empty()) {
            std::string text = std::format("old_string was not found in {} (an exact match is required)", display);
            if (looks_like_numbered(old_lf))
                text += "\nHint: every line of old_string starts with a line number and Tab from read output; "
                        "do not include line number prefixes.";
            else if (const auto line = unique_indent_insensitive(content, old_lf))
                text += std::format(
                    "\nHint: near line {} the text matches except for indentation. Rewrite "
                    "old_string with the file's exact indentation (suggestion only; not applied automatically).",
                    *line);
            else
                text += "\nHint: read this section again; spaces, punctuation and newlines must match exactly.";
            return error_result(std::move(text));
        }
        if (positions.size() > 1 && !replace_all_) {
            std::string lines;
            const std::size_t shown = std::min<std::size_t>(positions.size(), 20);
            for (std::size_t i = 0; i < shown; ++i)
                lines += (i == 0 ? "" : "、") + std::to_string(line_at(content, positions[i]));
            return error_result(std::format(
                "old_string matches {} at {} locations (lines: {}). Expand old_string to make it unique, or pass "
                "replace_all=true to replace all occurrences.",
                display, positions.size(), lines));
        }

        std::string new_content;
        new_content.reserve(content.size() + new_lf.size() * positions.size());
        std::size_t last = 0;
        for (const std::size_t pos : positions) {
            new_content.append(content, last, pos - last);
            new_content += new_lf;
            last = pos + old_lf.size();
        }
        new_content.append(content, last, content.size() - last);
        if (new_content == content)
            return error_result("replacement makes no change (new_string matches the original text)");
        // 写入上限在 prepare 就拦下：注定失败的调用不应该先弹一次确认
        if (new_content.size() > ctx.files().max_write_bytes)
            return error_result(std::format(
                "edited content is {} bytes; write size limit reached ({} bytes)", new_content.size(),
                ctx.files().max_write_bytes));

        const workspace::Unified diff = workspace::unified_diff(content, new_content, display);

        // 每处替换在 new 内容里的行区间（后续位置随前面的替换逐个平移）
        std::vector<Region> regions;
        const std::ptrdiff_t shift =
            static_cast<std::ptrdiff_t>(count_lines(new_lf)) - static_cast<std::ptrdiff_t>(count_lines(old_lf));
        std::ptrdiff_t delta = 0;
        for (const std::size_t pos : positions) {
            const std::size_t first = line_at(content, pos) + static_cast<std::size_t>(delta);
            regions.push_back({first, first + count_lines(new_lf) - 1});
            delta += shift;
        }
        std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) {
            return a.first < b.first;
        });

        std::string snippets;
        const std::size_t dropped = build_snippets(new_content, regions,
                                                   ctx.options().max_result_bytes - 128,
                                                   ctx.options().read_max_line_bytes, snippets);
        if (dropped > 0) snippets += std::format("({} additional changes not displayed)\n", dropped);

        agent::FileChangeView view;
        view.path = display;
        view.diff = diff.text;
        view.added = static_cast<int>(diff.stat.added);
        view.removed = static_cast<int>(diff.stat.removed);
        view.created = false;

        std::string success = std::format("Edited {} (+{} -{}).\n", display, diff.stat.added,
                                          diff.stat.removed);
        success += snippets;
        new_content_ = std::move(new_content);
        eol_ = file.eol;
        bom_ = file.bom;
        success_text_ = std::move(success);
        view_ = std::move(view);
        intent_.preview = view_.diff;
        intent_.summary = std::format("Edit {} (+{} -{})", path_, view_.added, view_.removed);
        ready_ = true;
        return std::nullopt;
}

} // namespace

std::unique_ptr<Tool> detail::make_edit_tool() { return std::make_unique<EditTool>(); }

} // namespace dagent::tools
