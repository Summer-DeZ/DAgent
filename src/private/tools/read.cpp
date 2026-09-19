#include <algorithm>
#include <filesystem>
#include <format>
#include <system_error>

#include "base/text.hpp"
#include "tools/detail.hpp"

namespace dagent::tools {
namespace fs = std::filesystem;
using detail::count_lines;
using detail::error_result;
using detail::fit_line;
using detail::get_int;
using detail::line_prefix;
using detail::resolve_arg;
using detail::split_lines;

namespace {

constexpr std::string_view kDescription = R"(读取工作区里的文本文件，或列出目录内容。

- 输出每行的格式为「行号 + Tab + 内容」，行号从 1 开始。
- 用 offset（起始行号）和 limit（行数）分页读取大文件；结尾的提示行说明文件总行数与已显示范围。
- 传入目录路径时列出直接子项（目录名带 / 后缀），不需要另外执行 ls。
- 二进制文件直接报错并给出大小；路径不存在时会给出几个相近的候选路径。
- 调用 edit 或 write 修改一个已有文件之前，必须先用本工具读过它。)";

/// 不存在的路径给出至多 3 个相近候选，帮模型自己纠正。
std::vector<std::string> suggest_similar(const Context& ctx, const std::string& raw) {
    try {
        workspace::FilesQuery query;
        query.root = ctx.root();
        query.max_files = 20000;
        const auto pool = workspace::files(query, ctx.search());
        std::string needle = detail::expand_home(raw);
        const auto pick = [&](std::string_view key) {
            std::vector<std::string> out;
            for (const std::size_t i : workspace::fuzzy_rank(key, pool, 3)) out.push_back(pool[i]);
            return out;
        };
        std::vector<std::string> picks = pick(needle);
        if (picks.empty()) {
            const auto slash = needle.find_last_of('/');
            if (slash != std::string::npos) picks = pick(needle.substr(slash + 1));
        }
        return picks;
    } catch (const workspace::WorkspaceError&) {
        return {}; // 搜索环境不可用时只少给提示，不影响错误本身
    }
}

class ReadCall final : public Call {
public:
    ReadCall(Context& ctx, workspace::Resolved target, std::string display, int offset, int limit,
             bool directory)
        : ctx_(ctx), target_(std::move(target)), display_(std::move(display)), offset_(offset),
          limit_(limit), directory_(directory) {
        intent_.kind = Intent::Kind::read;
        intent_.paths = {target_};
        intent_.summary = std::format("{} {}", directory_ ? "列目录" : "读取", display_);
    }

private:
    Result do_run(const Grant&, const std::function<void(std::string_view)>&, std::stop_token) override {
        return directory_ ? list_directory() : read_file();
    }

    Result list_directory() {
        std::vector<std::string> names;
        bool too_many = false, cut = false;
        std::error_code ec;
        fs::directory_iterator it(target_.path, fs::directory_options::skip_permission_denied, ec), end;
        if (ec) return error_result(std::format("无法读取目录 {}：{}", display_, ec.message()));
        for (; it != end && !ec; it.increment(ec)) {
            const bool is_dir = it->is_directory(ec);
            names.emplace_back(it->path().filename().string() + (!ec && is_dir ? "/" : ""));
            if (names.size() > max_files()) {
                names.pop_back();
                too_many = true;
                break;
            }
        }
        std::sort(names.begin(), names.end());
        std::string text;
        for (const std::string& name : names) {
            // 预算留给正文，末尾的状态行有固定余量
            if (text.size() + name.size() + 1 + 64 > ctx_.options().max_result_bytes) {
                cut = true;
                break;
            }
            text += name;
            text += '\n';
        }
        if (cut) text += "[目录内容超出预算，已截断]\n";
        if (too_many) text += std::format("[子项过多，已显示前 {} 项]\n", max_files());

        ReadView view;
        view.path = display_;
        view.directory = true;
        view.truncated = too_many || cut;
        Result result;
        result.text = base::to_valid_utf8(std::move(text));
        result.display = std::move(view);
        return result;
    }

    Result read_file() {
        const workspace::TextFile file = workspace::read_text(target_.path, ctx_.files());
        ReadView view;
        view.path = display_;
        view.truncated = file.truncated;

        std::string text;
        if (file.content.empty()) {
            text = "（空文件）\n";
        } else {
            const auto lines = split_lines(file.content);
            const std::size_t total = count_lines(file.content);
            view.total_lines = static_cast<int>(total);
            const std::size_t start = std::max(1, offset_);
            if (start > total) {
                text = std::format("[文件共 {} 行，offset {} 超出末尾，没有内容可显示]\n", total, start);
            } else {
                // 预算先留给正文，说明行放在最后并预留固定余量，保证它自己不会被截掉
                constexpr std::size_t kFooterRoom = 192;
                const std::size_t budget = ctx_.options().max_result_bytes;
                const std::size_t end = std::min(total, start + static_cast<std::size_t>(limit_) - 1);
                std::size_t used = 0;
                std::size_t shown_end = start - 1;
                bool cut = false;
                for (std::size_t i = start; i <= end; ++i) {
                    std::string line = line_prefix(i) + fit_line(lines[i - 1], max_line_bytes());
                    if (used + line.size() + 1 + kFooterRoom > budget) {
                        cut = true;
                        break;
                    }
                    used += line.size() + 1;
                    text += line;
                    text += '\n';
                    shown_end = i;
                }
                view.start_line = static_cast<int>(start);
                view.end_line = static_cast<int>(shown_end);
                if (cut)
                    text += std::format(
                        "[文件共 {} 行，已显示 {}–{} 行，结果超出预算被截断，用 offset 继续读]\n", total,
                        start, shown_end);
                else if (shown_end < total)
                    text += std::format("[文件共 {} 行，已显示 {}–{} 行，用 offset 继续读]\n", total, start,
                                        shown_end);
                else
                    text += std::format("[文件共 {} 行，已全部显示]\n", total);
            }
        }
        if (file.lossy) text += "[注意：文件含非法 UTF-8，已替换为 U+FFFD]\n";
        if (file.truncated)
            text += std::format("[注意：文件超过单次读取上限（{} 字节），内容不完整]\n",
                                ctx_.files().max_read_bytes);

        Result result;
        result.text = base::to_valid_utf8(std::move(text));
        result.display = std::move(view);
        ctx_.track(target_, file.stamp); // 部分读取也算读过
        return result;
    }

    std::size_t max_line_bytes() const { return ctx_.options().read_max_line_bytes; }
    std::size_t max_files() const { return ctx_.options().glob_max_files; }

    Context& ctx_;
    workspace::Resolved target_;
    std::string display_;
    int offset_ = 1, limit_ = 0;
    bool directory_ = false;
};

class ReadTool final : public Tool {
public:
    ReadTool() {
        spec_.name = "read";
        spec_.description = std::string(kDescription);
        spec_.parameters = {
            {"type", "object"},
            {"properties",
             {{"path", {{"type", "string"}, {"description", "文件或目录路径，相对工作区根，支持 ~/ 开头"}}},
              {"offset", {{"type", "integer"}, {"description", "起始行号（从 1 开始），默认 1"}}},
              {"limit", {{"type", "integer"}, {"description", "最多显示的行数，默认 2000"}}}}},
            {"required", std::vector<std::string>{"path"}},
        };
    }

    const Spec& spec() const override { return spec_; }

    std::expected<std::unique_ptr<Call>, Result> prepare(std::string_view arguments,
                                                         Context& ctx) const override {
        auto args = detail::parse_arguments(arguments);
        if (!args) return std::unexpected(error_result(args.error()));
        std::string err;
        const std::string path = detail::require_string(*args, "path", err);
        if (!err.empty()) return std::unexpected(error_result(err));
        const auto offset = get_int(*args, "offset", err);
        if (!err.empty()) return std::unexpected(error_result(err));
        const auto limit = get_int(*args, "limit", err);
        if (!err.empty()) return std::unexpected(error_result(err));

        const workspace::Resolved resolved = resolve_arg(ctx, path);
        const std::string display = detail::display_path(ctx, resolved);
        const workspace::FileKind kind = workspace::probe(resolved.path);

        if (kind == workspace::FileKind::missing) {
            std::string text = std::format("文件不存在：{}", display);
            if (const auto picks = suggest_similar(ctx, path); !picks.empty()) {
                text += "\n你要找的是不是：";
                for (const std::string& pick : picks) text += std::format("\n  {}", pick);
            }
            return std::unexpected(error_result(std::move(text)));
        }
        if (kind == workspace::FileKind::binary) {
            std::error_code ec;
            const auto size = fs::file_size(resolved.path, ec);
            return std::unexpected(error_result(std::format(
                "无法读取：{} 是二进制文件（{} 字节），不输出内容", display, ec ? 0 : size)));
        }
        const int start = offset && *offset > 0 ? static_cast<int>(*offset) : 1;
        const int lines = limit && *limit > 0 ? static_cast<int>(*limit) : ctx.options().read_default_lines;
        return std::make_unique<ReadCall>(ctx, resolved, display, start, lines,
                                          kind == workspace::FileKind::directory);
    }

private:
    Spec spec_;
};

} // namespace

std::unique_ptr<Tool> detail::make_read_tool() { return std::make_unique<ReadTool>(); }

} // namespace dagent::tools
