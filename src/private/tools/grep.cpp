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

constexpr std::string_view kDescription = R"(Search file contents in the workspace using ripgrep regular expressions.

- pattern is a Rust regular expression. Patterns without uppercase letters ignore case automatically (rg smart-case).
- path restricts the search to a directory or file (default: entire workspace). glob filters filenames with gitignore semantics, e.g. "*.cpp" or "!*.lock".
- Output uses path:line:content; context lines use path-line-content, and groups are separated by --.
- files_only=true returns one matching path per line, useful before reading the matching files.
- Results are bounded. If truncated, narrow the scope with a more precise pattern, deeper directory or files_only.)";

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
        intent_.kind = agent::ToolKind::read;
        intent_.paths = {to_intent(root_, agent::Access::read)};
        intent_.summary = std::format("Search {}", query_.pattern);
    }

private:
    Result do_run(const Grant&, const std::function<void(std::string_view)>&, std::stop_token stop) override {
        workspace::GrepResult found;
        try {
            found = workspace::grep(query_, search_options_, stop);
        } catch (const workspace::WorkspaceError& e) {
            if (e.kind() == workspace::WorkspaceError::Kind::cancelled) return interrupted_result();
            if (e.kind() == workspace::WorkspaceError::Kind::bad_pattern) // 模型的输入错误，不记 warn
                return error_result(std::format("invalid regular expression: {}", e.what()));
            throw;
        }
        for (workspace::Match& match : found.matches) match.path = path_prefix_ + match.path;

        constexpr std::size_t kNoteRoom = 64;
        agent::GrepView view;
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
                view.lines.push_back(agent::GrepLine{.path = match.path, .text = {}, .line = 0,
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
                view.lines.push_back(agent::GrepLine{.path = match.path, .text = match.text,
                                              .line = match.line, .spans = match.spans,
                                              .is_context = match.is_context});
                const std::string_view sep = match.is_context ? "-" : ":";
                if (!append(std::format("{}{}{}{}{}", match.path, sep, match.line, sep,
                                        match.text)))
                    break;
            }
        }
        if (overflow || found.truncated) text += "[Results truncated. Narrow the search scope.]\n";
        if (text.empty()) text = "(no matches)\n";

        Result result;
        result.model_text = base::to_valid_utf8(std::move(text));
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
        result.model_text = "interrupted by the user";
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
             {{"pattern", {{"type", "string"}, {"description", "Rust regular expression"}}},
              {"path", {{"type", "string"}, {"description", "Directory or file to search; defaults to the workspace root"}}},
              {"glob", {{"type", "string"}, {"description", "Filter filenames using gitignore semantics, e.g. \"*.cpp\""}}},
              {"type", {{"type", "string"}, {"description", "rg --type, e.g. cpp or py"}}},
              {"ignore_case", {{"type", "boolean"}, {"description", "Ignore case; defaults to smart-case"}}},
              {"context", {{"type", "integer"}, {"description", "Context lines before and after each match"}}},
              {"files_only", {{"type", "boolean"}, {"description", "List only matching file paths"}}}}},
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
                error_result(std::format("path does not exist: {}", detail::display_path(ctx, root))));
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
