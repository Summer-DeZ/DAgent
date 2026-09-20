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

class EditCall final : public Call {
public:
    EditCall(Context& ctx, workspace::Resolved target, std::string path, std::string new_content,
             workspace::Eol eol, bool bom, workspace::Stamp expect, std::string success_text,
             FileChangeView view)
        : ctx_(ctx), target_(std::move(target)), path_(std::move(path)),
          new_content_(std::move(new_content)), eol_(eol), bom_(bom), expect_(expect),
          success_text_(std::move(success_text)), view_(std::move(view)) {
        intent_.kind = Intent::Kind::write;
        intent_.paths = {target_};
        intent_.preview = view_.diff;
        intent_.summary = std::format("Edit {} (+{} -{})", path_, view_.added, view_.removed);
    }

private:
    Result do_run(const Grant&, const std::function<void(std::string_view)>&, std::stop_token) override {
        try {
            workspace::write_text(target_.path, new_content_, eol_, bom_, expect_, ctx_.files());
        } catch (const workspace::WorkspaceError& e) {
            if (e.kind() == workspace::WorkspaceError::Kind::stale)
                return error_result(std::format(
                    "{} is stale - it changed since your last read (possibly by bash or the user); read it again before editing", path_));
            throw;
        }
        if (const auto stamp = workspace::stamp_of(target_.path)) ctx_.track(target_, *stamp);
        Result result;
        result.text = success_text_;
        result.display = view_;
        return result;
    }

    Context& ctx_;
    workspace::Resolved target_;
    std::string path_;
    std::string new_content_;
    workspace::Eol eol_ = workspace::Eol::lf;
    bool bom_ = false;
    workspace::Stamp expect_;
    std::string success_text_;
    FileChangeView view_;
};

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

    std::expected<std::unique_ptr<Call>, Result> prepare(std::string_view arguments,
                                                         Context& ctx) const override {
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

        const auto tracked = ctx.tracked_stamp(resolved);
        if (!tracked)
            return std::unexpected(
                error_result(std::format("{} has not been read; use read before editing this file", display)));

        const workspace::FileKind kind = workspace::probe(resolved.path);
        if (kind == workspace::FileKind::missing)
            return std::unexpected(error_result(std::format(
                "{} no longer exists (deleted or moved since your read); check its current location", display)));
        if (kind == workspace::FileKind::directory)
            return std::unexpected(error_result(std::format("{} is a directory and cannot be edited", display)));

        if (const auto current = workspace::stamp_of(resolved.path); !current || !(*current == *tracked))
            return std::unexpected(error_result(std::format(
                "{} is stale - it changed since your last read (possibly by bash or the user); read it again before editing", display)));

        workspace::TextFile file;
        try {
            file = workspace::read_text(resolved.path, ctx.files());
        } catch (const workspace::WorkspaceError& e) {
            if (e.kind() == workspace::WorkspaceError::Kind::not_text)
                return std::unexpected(
                    error_result(std::format("{} is binary; edit cannot modify it", display)));
            throw;
        }
        if (file.lossy)
            return std::unexpected(error_result(std::format(
                "{} contains invalid UTF-8 (replaced with U+FFFD when read); editing is denied to avoid corrupting the original file", display)));
        if (file.truncated)
            return std::unexpected(error_result(std::format(
                "{} is larger than the per-read limit; only its beginning was read. Editing is denied to avoid losing the remaining content", display)));

        const std::string old_lf = detail::to_lf(*old_string);
        const std::string new_lf = detail::to_lf(*new_string);
        if (old_lf.empty()) return std::unexpected(error_result("old_string must not be empty"));
        if (old_lf == new_lf) return std::unexpected(error_result("old_string and new_string are identical"));

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
            return std::unexpected(error_result(std::move(text)));
        }
        if (positions.size() > 1 && !replace_all.value_or(false)) {
            std::string lines;
            const std::size_t shown = std::min<std::size_t>(positions.size(), 20);
            for (std::size_t i = 0; i < shown; ++i)
                lines += (i == 0 ? "" : "、") + std::to_string(line_at(content, positions[i]));
            return std::unexpected(error_result(std::format(
                "old_string matches {} at {} locations (lines: {}). Expand old_string to make it unique, or pass "
                "replace_all=true to replace all occurrences.",
                display, positions.size(), lines)));
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
            return std::unexpected(error_result("replacement makes no change (new_string matches the original text)"));
        // 写入上限在 prepare 就拦下：注定失败的调用不应该先弹一次确认
        if (new_content.size() > ctx.files().max_write_bytes)
            return std::unexpected(error_result(std::format(
                "edited content is {} bytes; write size limit reached ({} bytes)", new_content.size(),
                ctx.files().max_write_bytes)));

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

        FileChangeView view;
        view.path = display;
        view.diff = diff.text;
        view.added = static_cast<int>(diff.stat.added);
        view.removed = static_cast<int>(diff.stat.removed);
        view.created = false;

        std::string success = std::format("Edited {} (+{} -{}).\n", display, diff.stat.added,
                                          diff.stat.removed);
        success += snippets;
        return std::make_unique<EditCall>(ctx, resolved, display, std::move(new_content), file.eol,
                                          file.bom, *tracked, std::move(success), std::move(view));
    }

private:
    Spec spec_;
};

} // namespace

std::unique_ptr<Tool> detail::make_edit_tool() { return std::make_unique<EditTool>(); }

} // namespace dagent::tools
