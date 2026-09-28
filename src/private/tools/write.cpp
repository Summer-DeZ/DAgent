#include <format>
#include <utility>

#include "tools/detail.hpp"
#include "workspace/diff.hpp"

namespace dagent::tools {
using detail::count_lines;
using detail::error_result;
using detail::require_string;
using detail::resolve_arg;

namespace {

constexpr std::string_view kDescription = R"(Create a file or completely overwrite a file that has already been read.

- New files and parent directories are created automatically, using UTF-8 with LF newlines and no BOM.
- Before overwriting an existing file, you must read it. Its newline style and BOM are preserved; overwriting an unread file is denied.
- Do not include read output's line number prefixes. Prefer edit when changing only part of a file.)";

class WriteCall final : public PreparedTool {
public:
    WriteCall(Context& ctx, workspace::Resolved target,
              std::string path, std::string content, workspace::Eol eol, bool bom,
              std::optional<workspace::Stamp> expect, std::string success_text,
              agent::FileChangeView view)
        : ctx_(ctx), target_(std::move(target)), path_(std::move(path)),
          content_(std::move(content)), eol_(eol), bom_(bom), expect_(expect),
          success_text_(std::move(success_text)), view_(std::move(view)) {
        intent_.kind = agent::ToolKind::write;
        intent_.paths = {to_intent(target_, agent::Access::write)};
        intent_.preview = view_.diff;
        intent_.summary = std::format("{} {} (+{} -{})", view_.created ? "Create" : "Overwrite", path_,
                                      view_.added, view_.removed);
    }

private:
    Result do_execute(const Grant&, const std::function<void(std::string_view)>&, std::stop_token) override {
        try {
            workspace::write_text(target_.path, content_, eol_, bom_, expect_, ctx_.files());
        } catch (const workspace::WorkspaceError& e) {
            if (e.kind() == workspace::WorkspaceError::Kind::stale)
                return error_result(std::format(
                    "{} is stale - it changed since your last read (possibly by bash or the user); read it again before editing", path_));
            throw;
        }
        if (const auto stamp = workspace::stamp_of(target_.path)) ctx_.track(target_, *stamp);
        Result result;
        result.model_text = success_text_;
        result.display = view_;
        return result;
    }

    Context& ctx_;
    workspace::Resolved target_;
    std::string path_;
    std::string content_;
    workspace::Eol eol_ = workspace::Eol::lf;
    bool bom_ = false;
    std::optional<workspace::Stamp> expect_;
    std::string success_text_;
    agent::FileChangeView view_;
};

class WriteTool final : public Tool {
public:
    WriteTool() {
        spec_.name = "write";
        spec_.description = std::string(kDescription);
        spec_.parameters = {
            {"type", "object"},
            {"properties",
             {{"path", {{"type", "string"}, {"description", "Target file path relative to the workspace root; supports ~/"}}},
              {"content", {{"type", "string"}, {"description", "Complete content to write"}}}}},
            {"required", std::vector<std::string>{"path", "content"}},
        };
    }

    const Spec& spec() const override { return spec_; }

    std::expected<std::unique_ptr<PreparedTool>, Result> prepare(
        std::string_view arguments, Context& ctx) const override {
        auto args = detail::parse_arguments(arguments);
        if (!args) return std::unexpected(error_result(args.error()));
        std::string err;
        const std::string path = require_string(*args, "path", err);
        const auto content = detail::get_string(*args, "content", err);
        if (!err.empty()) return std::unexpected(error_result(err));
        if (!content) return std::unexpected(error_result("content is required"));

        const std::string new_content = detail::to_lf(*content);
        const workspace::FileOptions& files = ctx.files();
        if (new_content.size() > files.max_write_bytes)
            return std::unexpected(error_result(std::format(
                "content is {} bytes; write size limit reached ({} bytes)", new_content.size(), files.max_write_bytes)));

        const workspace::Resolved resolved = resolve_arg(ctx, path);
        const std::string display = detail::display_path(ctx, resolved);
        const workspace::FileKind kind = workspace::probe(resolved.path);
        if (kind == workspace::FileKind::directory)
            return std::unexpected(error_result(std::format("{} is a directory and cannot be written", display)));

        workspace::Eol eol = workspace::Eol::lf;
        bool bom = false;
        std::optional<workspace::Stamp> expect;
        std::string before;
        const bool existed = kind != workspace::FileKind::missing;

        if (existed) {
            const auto tracked = ctx.tracked_stamp(resolved);
            if (!tracked)
                return std::unexpected(error_result(std::format(
                    "{} already exists; use read before overwriting it (not required for new files)", display)));
            if (const auto current = workspace::stamp_of(resolved.path);
                !current || !(*current == *tracked))
                return std::unexpected(error_result(std::format(
                    "{} is stale - it changed since your last read (possibly by bash or the user); read it again before editing", display)));
            workspace::TextFile file;
            try {
                file = workspace::read_text(resolved.path, files);
            } catch (const workspace::WorkspaceError& e) {
                if (e.kind() == workspace::WorkspaceError::Kind::not_text)
                    return std::unexpected(
                        error_result(std::format("{} is binary; write cannot overwrite it", display)));
                throw;
            }
            eol = file.eol;
            bom = file.bom;
            expect = tracked;
            before = std::move(file.content);
        }

        const workspace::Unified diff = workspace::unified_diff(before, new_content, display);

        agent::FileChangeView view;
        view.path = display;
        view.diff = diff.text;
        view.added = static_cast<int>(diff.stat.added);
        view.removed = static_cast<int>(diff.stat.removed);
        view.created = !existed;

        const std::string success =
            existed ? std::format("Overwrote {} (+{} -{}).", display, diff.stat.added, diff.stat.removed)
                    : std::format("Created {} ({} lines).", display, count_lines(new_content));
        return std::make_unique<WriteCall>(ctx, resolved, display, new_content, eol, bom,
                                           expect, success, std::move(view));
    }

private:
    Spec spec_;
};

} // namespace

std::unique_ptr<Tool> detail::make_write_tool() { return std::make_unique<WriteTool>(); }

} // namespace dagent::tools
