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

constexpr std::string_view kDescription = R"(新建文件，或整体覆盖一个已读过的文件。

- 新文件直接创建，父目录自动建立，内容按 UTF-8（LF 换行、无 BOM）写入。
- 覆盖已有文件之前必须先用 read 读过它，并保留原有的换行符风格与 BOM；没读过会被拒绝。
- 内容不要带 read 输出的行号前缀。要改文件的一部分请优先用 edit。)";

class WriteCall final : public Call {
public:
    WriteCall(Context& ctx, workspace::Resolved target, std::string path, std::string content,
              workspace::Eol eol, bool bom, std::optional<workspace::Stamp> expect,
              std::string success_text, FileChangeView view)
        : ctx_(ctx), target_(std::move(target)), path_(std::move(path)),
          content_(std::move(content)), eol_(eol), bom_(bom), expect_(expect),
          success_text_(std::move(success_text)), view_(std::move(view)) {
        intent_.kind = Intent::Kind::write;
        intent_.paths = {target_};
        intent_.preview = view_.diff;
        intent_.summary = std::format("{} {}（+{} −{}）", view_.created ? "创建" : "覆盖", path_,
                                      view_.added, view_.removed);
    }

private:
    Result do_run(const Grant&, const std::function<void(std::string_view)>&, std::stop_token) override {
        try {
            workspace::write_text(target_.path, content_, eol_, bom_, expect_, ctx_.files());
        } catch (const workspace::WorkspaceError& e) {
            if (e.kind() == workspace::WorkspaceError::Kind::stale)
                return error_result(std::format(
                    "文件 {} 在你上次读取后被修改过（可能是 bash 或用户改的），请重新 read", path_));
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
    std::string content_;
    workspace::Eol eol_ = workspace::Eol::lf;
    bool bom_ = false;
    std::optional<workspace::Stamp> expect_;
    std::string success_text_;
    FileChangeView view_;
};

class WriteTool final : public Tool {
public:
    WriteTool() {
        spec_.name = "write";
        spec_.description = std::string(kDescription);
        spec_.parameters = {
            {"type", "object"},
            {"properties",
             {{"path", {{"type", "string"}, {"description", "目标文件路径，相对工作区根，支持 ~/ 开头"}}},
              {"content", {{"type", "string"}, {"description", "写入的完整内容"}}}}},
            {"required", std::vector<std::string>{"path", "content"}},
        };
    }

    const Spec& spec() const override { return spec_; }

    std::expected<std::unique_ptr<Call>, Result> prepare(std::string_view arguments,
                                                         Context& ctx) const override {
        auto args = detail::parse_arguments(arguments);
        if (!args) return std::unexpected(error_result(args.error()));
        std::string err;
        const std::string path = require_string(*args, "path", err);
        const auto content = detail::get_string(*args, "content", err);
        if (!err.empty()) return std::unexpected(error_result(err));
        if (!content) return std::unexpected(error_result("参数 content 缺失（必填）"));

        const std::string new_content = detail::to_lf(*content);
        const workspace::FileOptions& files = ctx.files();
        if (new_content.size() > files.max_write_bytes)
            return std::unexpected(error_result(std::format(
                "内容有 {} 字节，超过 write 上限（{} 字节）", new_content.size(), files.max_write_bytes)));

        const workspace::Resolved resolved = resolve_arg(ctx, path);
        const std::string display = detail::display_path(ctx, resolved);
        const workspace::FileKind kind = workspace::probe(resolved.path);
        if (kind == workspace::FileKind::directory)
            return std::unexpected(error_result(std::format("{} 是目录，不能写入", display)));

        workspace::Eol eol = workspace::Eol::lf;
        bool bom = false;
        std::optional<workspace::Stamp> expect;
        std::string before;
        const bool existed = kind != workspace::FileKind::missing;

        if (existed) {
            const auto tracked = ctx.tracked_stamp(resolved);
            if (!tracked)
                return std::unexpected(error_result(std::format(
                    "文件 {} 已存在：必须先用 read 读过才能覆盖（新建文件不需要）", display)));
            if (const auto current = workspace::stamp_of(resolved.path);
                !current || !(*current == *tracked))
                return std::unexpected(error_result(std::format(
                    "文件 {} 在你上次读取后被修改过（可能是 bash 或用户改的），请重新 read", display)));
            workspace::TextFile file;
            try {
                file = workspace::read_text(resolved.path, files);
            } catch (const workspace::WorkspaceError& e) {
                if (e.kind() == workspace::WorkspaceError::Kind::not_text)
                    return std::unexpected(
                        error_result(std::format("{} 是二进制文件，拒绝用 write 覆盖", display)));
                throw;
            }
            eol = file.eol;
            bom = file.bom;
            expect = tracked;
            before = std::move(file.content);
        }

        const workspace::Unified diff = workspace::unified_diff(before, new_content, display);

        FileChangeView view;
        view.path = display;
        view.diff = diff.text;
        view.added = static_cast<int>(diff.stat.added);
        view.removed = static_cast<int>(diff.stat.removed);
        view.created = !existed;

        const std::string success =
            existed ? std::format("已覆盖 {}（+{} −{}）", display, diff.stat.added, diff.stat.removed)
                    : std::format("已创建 {}（{} 行）", display, count_lines(new_content));
        return std::make_unique<WriteCall>(ctx, resolved, display, new_content, eol, bom, expect,
                                           success, std::move(view));
    }

private:
    Spec spec_;
};

} // namespace

std::unique_ptr<Tool> detail::make_write_tool() { return std::make_unique<WriteTool>(); }

} // namespace dagent::tools
