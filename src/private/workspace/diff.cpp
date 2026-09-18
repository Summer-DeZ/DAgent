#include "workspace/diff.hpp"

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

#include "lib/dtl/dtl.hpp"

namespace dagent::workspace {
namespace {

// 每个元素保留行尾的 '\n'：最后一行没有换行符时它天然和 "有换行" 的行不相等，
// 输出时据此补上 "\ No newline at end of file"。
std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t newline = text.find('\n', start);
        if (newline == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, newline - start + 1));
        start = newline + 1;
    }
    return lines;
}

void append_line(std::string& out, char prefix, std::string_view line) {
    out += prefix;
    out.append(line);
    if (line.empty() || line.back() != '\n') out += "\n\\ No newline at end of file\n";
}

void append_file_header(std::string& out, std::string_view path, bool exists_before, bool exists_after) {
    out += std::format("--- {}\n", exists_before ? std::format("a/{}", path) : "/dev/null");
    out += std::format("+++ {}\n", exists_after ? std::format("b/{}", path) : "/dev/null");
}

struct Op {
    dtl::edit_t type;
    std::string_view line;
    std::size_t old_before; ///< 这一行之前旧文件已经有多少行
    std::size_t new_before;
};

} // namespace

Unified unified_diff(std::string_view before, std::string_view after, std::string_view path,
                     const DiffOptions& opt) {
    Unified out;
    if (before == after) return out;

    const std::vector<std::string_view> old_lines = split_lines(before);
    const std::vector<std::string_view> new_lines = split_lines(after);

    append_file_header(out.text, path, !old_lines.empty(), !new_lines.empty());

    if (old_lines.size() > opt.max_lines && new_lines.size() > opt.max_lines) {
        out.whole_file = true;
        out.stat = {new_lines.size(), old_lines.size()};
        out.text += std::format("@@ -{},{} +{},{} @@\n", old_lines.empty() ? 0 : 1, old_lines.size(),
                                new_lines.empty() ? 0 : 1, new_lines.size());
        for (const std::string_view line : old_lines) append_line(out.text, '-', line);
        for (const std::string_view line : new_lines) append_line(out.text, '+', line);
        return out;
    }

    dtl::Diff<std::string_view> diff(old_lines, new_lines);
    diff.compose();

    std::vector<Op> ops;
    std::size_t old_pos = 0;
    std::size_t new_pos = 0;
    for (const auto& [line, info] : diff.getSes().getSequence()) {
        ops.push_back({info.type, line, old_pos, new_pos});
        if (info.type == dtl::SES_DELETE) {
            ++old_pos;
            ++out.stat.removed;
        } else if (info.type == dtl::SES_ADD) {
            ++new_pos;
            ++out.stat.added;
        } else {
            ++old_pos;
            ++new_pos;
        }
    }

    // 把相邻改动合并成 hunk：改动前后各留 context 行；两个 hunk 的上下文相接时合并。
    const std::size_t context = static_cast<std::size_t>(std::max(opt.context, 0));
    std::vector<std::pair<std::size_t, std::size_t>> hunks;
    for (std::size_t i = 0; i < ops.size();) {
        if (ops[i].type == dtl::SES_COMMON) {
            ++i;
            continue;
        }
        std::size_t end_change = i;
        while (end_change < ops.size() && ops[end_change].type != dtl::SES_COMMON) ++end_change;
        const std::size_t start = i > context ? i - context : 0;
        const std::size_t end = std::min(ops.size(), end_change + context);
        if (!hunks.empty() && start <= hunks.back().second) {
            hunks.back().second = std::max(hunks.back().second, end);
        } else {
            hunks.push_back({start, end});
        }
        i = end_change;
    }

    for (const auto& [start, end] : hunks) {
        std::size_t old_count = 0;
        std::size_t new_count = 0;
        for (std::size_t k = start; k < end; ++k) {
            if (ops[k].type != dtl::SES_ADD) ++old_count;
            if (ops[k].type != dtl::SES_DELETE) ++new_count;
        }
        const std::size_t old_start = old_count ? ops[start].old_before + 1 : ops[start].old_before;
        const std::size_t new_start = new_count ? ops[start].new_before + 1 : ops[start].new_before;
        out.text += std::format("@@ -{},{} +{},{} @@\n", old_start, old_count, new_start, new_count);

        std::vector<std::string_view> deletes;
        std::vector<std::string_view> adds;
        const auto flush = [&] {
            for (const std::string_view line : deletes) append_line(out.text, '-', line);
            for (const std::string_view line : adds) append_line(out.text, '+', line);
            deletes.clear();
            adds.clear();
        };
        for (std::size_t k = start; k < end; ++k) {
            const Op& op = ops[k];
            if (op.type == dtl::SES_COMMON) {
                flush();
                append_line(out.text, ' ', op.line);
            } else if (op.type == dtl::SES_DELETE) {
                deletes.push_back(op.line);
            } else {
                adds.push_back(op.line);
            }
        }
        flush();
    }
    return out;
}

} // namespace dagent::workspace
