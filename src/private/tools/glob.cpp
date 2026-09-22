#include <format>
#include <utility>

#include "base/text.hpp"
#include "tools/detail.hpp"

namespace dagent::tools {
using detail::error_result;
using detail::require_string;
using detail::resolve_arg;

namespace {

constexpr std::string_view kDescription = R"(List workspace files using a glob pattern with gitignore semantics.

- Patterns match at any depth: *.cpp can match src/a/b.cpp. Use patterns such as src/**/*.hpp to narrow the scope.
- Results are sorted with the most recently modified first. Hidden files and files ignored by .gitignore are excluded.
- Results are bounded. If truncated, use a more specific pattern.)";

class GlobCall final : public Call {
public:
    GlobCall(const Context& ctx, workspace::Resolved root, std::string pattern)
        : root_(std::move(root)), search_options_(ctx.search()), pattern_(std::move(pattern)),
          max_files_(ctx.options().glob_max_files), max_result_bytes_(ctx.options().max_result_bytes) {
        // workspace::files 返回的路径相对查询根；拼上前缀才是相对工作区根、模型能直接 read 的路径
        path_prefix_ = detail::relative_prefix(root_.path, ctx.root());
        intent_.kind = agent::ToolKind::read;
        intent_.paths = {to_intent(root_, agent::Access::read)};
        intent_.summary = std::format("List {}", pattern_);
    }

private:
    Result do_run(const Grant&, const std::function<void(std::string_view)>&, std::stop_token stop) override {
        workspace::FilesQuery query;
        query.root = root_.path;
        query.globs = {pattern_};
        query.sort_by_mtime = true;
        query.max_files = max_files_;
        std::vector<std::string> found;
        try {
            found = workspace::files(query, search_options_, stop);
        } catch (const workspace::WorkspaceError& e) {
            if (e.kind() == workspace::WorkspaceError::Kind::cancelled) {
                Result result;
                result.model_text = "interrupted by the user";
                result.interrupted = true;
                return result;
            }
            if (e.kind() == workspace::WorkspaceError::Kind::bad_pattern) // 模型的输入错误，不记 warn
                return error_result(std::format("invalid glob pattern: {}", e.what()));
            throw;
        }
        for (std::string& file : found) file = path_prefix_ + file;

        const bool truncated = found.size() >= max_files_;
        std::string text;
        for (const std::string& file : found) {
            if (text.size() + file.size() + 1 + 64 > max_result_bytes_) {
                text += "[Results truncated. Use a more specific pattern.]\n";
                break;
            }
            text += file;
            text += '\n';
        }
        if (truncated && text.size() + 64 + 64 <= max_result_bytes_)
            text += "[Result count limit reached; results may be incomplete.]\n";
        if (text.empty()) text = "(no matching files)\n";

        agent::GlobView view;
        view.pattern = pattern_;
        view.files = found;
        view.truncated = truncated;
        Result result;
        result.model_text = base::to_valid_utf8(std::move(text));
        result.display = std::move(view);
        return result;
    }

    workspace::Resolved root_;
    workspace::SearchOptions search_options_;
    std::string pattern_;
    std::size_t max_files_ = 0, max_result_bytes_ = 0;
    std::string path_prefix_;
};

class GlobTool final : public Tool {
public:
    GlobTool() {
        spec_.name = "glob";
        spec_.description = std::string(kDescription);
        spec_.parameters = {
            {"type", "object"},
            {"properties",
             {{"pattern", {{"type", "string"}, {"description", "Glob pattern with gitignore semantics, e.g. src/**/*.hpp"}}},
              {"path", {{"type", "string"}, {"description", "Directory to search from; defaults to the workspace root"}}}}},
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
        const auto path = detail::get_string(*args, "path", err);
        if (!err.empty()) return std::unexpected(error_result(err));

        workspace::Resolved root = resolve_arg(ctx, path.value_or("."));
        switch (workspace::probe(root.path)) {
        case workspace::FileKind::directory: break;
        case workspace::FileKind::missing:
            return std::unexpected(
                error_result(std::format("path does not exist: {}", detail::display_path(ctx, root))));
        default:
            return std::unexpected(error_result(std::format(
                "path must be a directory; {} is a file. Use read to see its contents", detail::display_path(ctx, root))));
        }
        return std::make_unique<GlobCall>(ctx, std::move(root), pattern);
    }

private:
    Spec spec_;
};

} // namespace

std::unique_ptr<Tool> detail::make_glob_tool() { return std::make_unique<GlobTool>(); }

} // namespace dagent::tools
