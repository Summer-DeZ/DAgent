#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <system_error>
#include <utility>

#include "base/text.hpp"
#include "tools/detail.hpp"

namespace dagent::tools {
using detail::error_result;
using detail::get_bool;
using detail::get_int;
using detail::get_string;
using detail::require_string;
using detail::resolve_arg;

namespace {

constexpr std::string_view kDescription = R"(用 ripgrep 在工作区里按正则搜索文件内容。

- pattern 是 Rust 正则；只要 pattern 里没有大写字母就自动忽略大小写（rg 的 smart-case）。
- path 限定搜索的目录或单个文件（默认整个工作区）；glob 用 gitignore 语义过滤文件名，如 "*.cpp" 或 "!*.lock"。
- 默认输出「路径:行号:内容」，带上下文时上下文行用「路径-行号-内容」，组与组之间用 -- 分隔。
- files_only=true 时每行一个文件路径，适合先看哪些文件命中再 read。
- 结果有上限，被截断时请缩小范围（更精确的 pattern、更深的目录、files_only）。)";

class GrepCall final : public Call {
public:
    GrepCall(const Context& ctx, workspace::Resolved root, workspace::GrepQuery query,
             bool files_only)
        : root_(std::move(root)), search_options_(ctx.search()), files_only_(files_only),
          max_result_bytes_(ctx.options().max_result_bytes), query_(std::move(query)) {
        // workspace::grep 返回的路径相对查询根（查询根是文件时相对它所在的目录）；
        // 拼上前缀才是相对工作区根、模型能直接 read 的路径
        std::error_code ec;
        const bool single_file = !std::filesystem::is_directory(root_.path, ec);
        path_prefix_ = detail::relative_prefix(single_file ? root_.path.parent_path() : root_.path, ctx.root());
        intent_.kind = Intent::Kind::read;
        intent_.paths = {root_};
        intent_.summary = std::format("搜索 {}", query_.pattern);
    }

private:
    Result do_run(const Grant&, const std::function<void(std::string_view)>&, std::stop_token stop) override {
        workspace::GrepResult found;
        try {
            found = workspace::grep(query_, search_options_, stop);
        } catch (const workspace::WorkspaceError& e) {
            if (e.kind() == workspace::WorkspaceError::Kind::cancelled) return interrupted_result();
            if (e.kind() == workspace::WorkspaceError::Kind::bad_pattern) // 模型的输入错误，不记 warn
                return error_result(std::format("正则表达式有问题：{}", e.what()));
            throw;
        }
        for (workspace::Match& match : found.matches) match.path = path_prefix_ + match.path;

        constexpr std::size_t kNoteRoom = 64;
        GrepView view;
        view.pattern = query_.pattern;
        view.truncated = found.truncated;

        std::string text;
        bool overflow = false;
        const auto append = [&](std::string line) {
            if (text.size() + line.size() + 1 + kNoteRoom > max_result_bytes_) {
                overflow = true;
                return false;
            }
            text += line;
            text += '\n';
            return true;
        };

        std::string last_path, last_file;
        std::uint64_t last_line = 0;
        if (files_only_) {
            for (const workspace::Match& match : found.matches) {
                if (match.path == last_file) continue;
                last_file = match.path;
                view.lines.push_back(GrepLine{.path = match.path, .text = {}, .line = 0,
                                              .spans = {}, .is_context = false});
                if (!append(match.path)) break;
            }
        } else {
            for (const workspace::Match& match : found.matches) {
                if (!text.empty() &&
                    (match.path != last_path || match.line > last_line + 1) &&
                    !append("--"))
                    break;
                last_path = match.path;
                last_line = match.line;
                view.lines.push_back(GrepLine{.path = match.path, .text = match.text,
                                              .line = match.line, .spans = match.spans,
                                              .is_context = match.is_context});
                const std::string_view sep = match.is_context ? "-" : ":";
                if (!append(std::format("{}{}{}{}{}", match.path, sep, match.line, sep,
                                        match.text)))
                    break;
            }
        }
        if (overflow || found.truncated) text += "[结果已截断，请缩小范围]\n";
        if (text.empty()) text = "（无匹配）\n";

        Result result;
        result.text = base::to_valid_utf8(std::move(text));
        result.display = std::move(view);
        return result;
    }

    workspace::Resolved root_;
    workspace::SearchOptions search_options_;
    bool files_only_ = false;
    std::size_t max_result_bytes_ = 0;
    workspace::GrepQuery query_;
    std::string path_prefix_;

    static Result interrupted_result() {
        Result result;
        result.text = "已被用户中断";
        result.interrupted = true;
        return result;
    }
};

class GrepTool final : public Tool {
public:
    GrepTool() {
        spec_.name = "grep";
        spec_.description = std::string(kDescription);
        spec_.parameters = {
            {"type", "object"},
            {"properties",
             {{"pattern", {{"type", "string"}, {"description", "Rust 正则表达式"}}},
              {"path", {{"type", "string"}, {"description", "搜索的目录或文件，默认工作区根"}}},
              {"glob", {{"type", "string"}, {"description", "按 gitignore 语义过滤文件名，如 \"*.cpp\""}}},
              {"type", {{"type", "string"}, {"description", "rg 的 --type，如 cpp、py"}}},
              {"ignore_case", {{"type", "boolean"}, {"description", "忽略大小写；默认 smart-case"}}},
              {"context", {{"type", "integer"}, {"description", "每个匹配前后附带的上下文行数"}}},
              {"files_only", {{"type", "boolean"}, {"description", "只列出命中的文件路径"}}}}},
            {"required", std::vector<std::string>{"pattern"}},
        };
    }

    const Spec& spec() const override { return spec_; }

    std::expected<std::unique_ptr<Call>, Result> prepare(std::string_view arguments,
                                                         Context& ctx) const override {
        auto args = detail::parse_arguments(arguments);
        if (!args) return std::unexpected(error_result(args.error()));
        std::string err;
        const std::string pattern = require_string(*args, "pattern", err);
        const auto path = get_string(*args, "path", err);
        const auto glob = get_string(*args, "glob", err);
        const auto type = get_string(*args, "type", err);
        const auto ignore_case = get_bool(*args, "ignore_case", err);
        const auto context = get_int(*args, "context", err);
        const auto files_only = get_bool(*args, "files_only", err);
        if (!err.empty()) return std::unexpected(error_result(err));

        workspace::Resolved root = resolve_arg(ctx, path.value_or("."));
        if (workspace::probe(root.path) == workspace::FileKind::missing)
            return std::unexpected(
                error_result(std::format("路径不存在：{}", detail::display_path(ctx, root))));
        workspace::GrepQuery query;
        query.pattern = pattern;
        query.root = root.path;
        if (glob) query.globs.push_back(*glob);
        if (type) query.type = *type;
        if (ignore_case) query.case_insensitive = *ignore_case;
        if (context && *context > 0) query.context = static_cast<int>(std::min<std::int64_t>(*context, 10));
        query.max_matches = ctx.options().grep_max_matches;

        return std::make_unique<GrepCall>(ctx, root, std::move(query), files_only.value_or(false));
    }

private:
    Spec spec_;
};

} // namespace

std::unique_ptr<Tool> detail::make_grep_tool() { return std::make_unique<GrepTool>(); }

} // namespace dagent::tools
