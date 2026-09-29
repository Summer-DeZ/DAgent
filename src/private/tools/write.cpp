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
    WriteCall(Context& ctx, workspace::Resolved target, std::string path, std::string content)
        : ctx_(ctx), target_(std::move(target)), reference_(target_.path), path_(std::move(path)),
          content_(std::move(content)) {
        intent_.kind = agent::ToolKind::write;
        intent_.paths = {to_intent(target_, agent::Access::write)};
        intent_.summary = "Write " + path_;
        if (reference_.kind() == workspace::FileKind::directory)
            throw workspace::WorkspaceError(workspace::WorkspaceError::Kind::io, "cannot write a directory");
        if (reference_.stamp()) {
            expect_ = ctx_.tracked_stamp(target_);
            if (!expect_ || expect_ != reference_.stamp())
                throw workspace::WorkspaceError(workspace::WorkspaceError::Kind::stale,
                                                "read the current file before overwriting it");
        } else {
            update_preview("");
            ready_ = true;
        }
    }
    std::optional<agent::PreparedIntent> preview_request() const override {
        if (ready_) return std::nullopt;
        agent::PreparedIntent request;
        request.kind = agent::ToolKind::read;
        request.paths = {to_intent(target_, agent::Access::read)};
        request.summary = "Read " + path_ + " for write preview";
        return request;
    }
private:
    void update_preview(std::string_view before) {
        const auto diff = workspace::unified_diff(before, content_, path_);
        view_.path = path_;
        view_.diff = diff.text;
        view_.added = static_cast<int>(diff.stat.added);
        view_.removed = static_cast<int>(diff.stat.removed);
        view_.created = !expect_;
        intent_.preview = view_.diff;
        intent_.summary = std::format("{} {} (+{} -{})", view_.created ? "Create" : "Overwrite",
                                      path_, view_.added, view_.removed);
    }
    std::optional<Result> do_prepare_preview(const Grant& grant) override {
        ctx_.require_access(grant, target_, agent::Access::read);
        const auto file = reference_.read(ctx_.files());
        if (file.stamp != *expect_) return error_result("file changed since read; read it again");
        if (file.truncated || file.lossy) return error_result("cannot preview incomplete or invalid text");
        eol_ = file.eol;
        bom_ = file.bom;
        update_preview(file.content);
        ready_ = true;
        return std::nullopt;
    }
    Result do_execute(const Grant& grant, const std::function<void(std::string_view)>&, std::stop_token) override {
        ctx_.require_access(grant, target_, agent::Access::write);
        if (!ready_) return error_result("write preview has not been authorized");
        ctx_.track(target_, reference_.write(content_, eol_, bom_, expect_, ctx_.files()));
        Result result;
        result.model_text = std::format("{} {}.", view_.created ? "Created" : "Overwrote", path_);
        result.display = view_;
        return result;
    }
    Context& ctx_;
    workspace::Resolved target_;
    workspace::FileReference reference_;
    std::string path_, content_;
    workspace::Eol eol_ = workspace::Eol::lf;
    bool bom_ = false, ready_ = false;
    std::optional<workspace::Stamp> expect_;
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
        return std::make_unique<WriteCall>(ctx, resolved, display, new_content);
    }

private:
    Spec spec_;
};

} // namespace

std::unique_ptr<Tool> detail::make_write_tool() { return std::make_unique<WriteTool>(); }

} // namespace dagent::tools
