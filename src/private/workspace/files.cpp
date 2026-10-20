#include "workspace/files.hpp"
#include "workspace/files_detail.hpp"

#include <system_error>

#include "base/text.hpp"

namespace dagent::workspace {
namespace fs = std::filesystem;
namespace {

// 前缀按路径分量逐个比较；路径都已经规范化，所以不会出现 "/work2" 被当成 "/work" 子路径。
bool is_within(const fs::path& root, const fs::path& p) {
    auto r = root.begin();
    auto q = p.begin();
    for (; r != root.end() && q != p.end(); ++r, ++q) {
        if (*r != *q) return false;
    }
    return r == root.end();
}

Eol detect_eol(std::string_view raw) {
    std::size_t crlf = 0, lf = 0, cr = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            if (i + 1 < raw.size() && raw[i + 1] == '\n') {
                ++crlf;
                ++i;
            } else {
                ++cr;
            }
        } else if (raw[i] == '\n') {
            ++lf;
        }
    }
    if (crlf + lf + cr == 0) return Eol::none;
    if (crlf > 0 && lf == 0 && cr == 0) return Eol::crlf;
    if (lf > 0 && crlf == 0 && cr == 0) return Eol::lf;
    return Eol::mixed;
}

std::string to_lf(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\r') {
            out += raw[i];
            continue;
        }
        out += '\n';
        if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
    }
    return out;
}

} // namespace

Resolved resolve(const fs::path& root, std::string_view user_path) {
    std::error_code ec;
    fs::path abs_root = fs::weakly_canonical(root, ec);
    if (ec) {
        ec.clear();
        abs_root = fs::absolute(root, ec).lexically_normal();
    }
    fs::path candidate = user_path.empty() ? abs_root : fs::path(user_path);
    if (!candidate.is_absolute()) candidate = abs_root / candidate;
    fs::path resolved = fs::weakly_canonical(candidate, ec);
    if (ec) {
        ec.clear();
        resolved = fs::absolute(candidate, ec).lexically_normal();
    }
    return {resolved, is_within(abs_root, resolved)};
}

namespace detail {

[[noreturn]] void fail(WorkspaceError::Kind kind, std::string_view op, const fs::path& p,
                       const std::string& detail) {
    std::string what = std::string(op) + ": " + p.string();
    if (!detail.empty()) what += ": " + detail;
    throw WorkspaceError(kind, what);
}

// 目标本身是符号链接时，落到它指向的真实文件上，而不是把链接换成普通文件。
fs::path through_symlink(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_symlink(p, ec)) return p;
    const fs::path link = fs::read_symlink(p, ec);
    if (ec) return p;
    const fs::path target = link.is_absolute() ? link : p.parent_path() / link;
    const fs::path resolved = fs::weakly_canonical(target, ec);
    return ec ? target.lexically_normal() : resolved;
}

void decode_text(std::string& raw, const fs::path& p, TextFile& out) {
    if (raw.find('\0') != std::string::npos) fail(WorkspaceError::Kind::not_text, "read_text", p, "binary content");

    if (raw.size() >= 3 && raw[0] == '\xEF' && raw[1] == '\xBB' && raw[2] == '\xBF') {
        out.bom = true;
        raw.erase(0, 3);
    }
    out.eol = detect_eol(raw);
    std::string lf = to_lf(raw);
    if (base::is_valid_utf8(lf)) {
        out.content = std::move(lf);
    } else {
        out.content = base::to_valid_utf8(lf, &out.lossy);
    }
}

std::string encode_text(std::string_view content, Eol eol, bool bom) {
    std::string data;
    if (bom) data += "\xEF\xBB\xBF";
    data.reserve(data.size() + content.size() + content.size() / 16 + 16);
    if (eol == Eol::crlf) {
        for (const char c : content) {
            if (c == '\n') data += '\r';
            data += c;
        }
    } else {
        data.append(content);
    }
    return data;
}

std::string encode_reference_text(std::string_view content, Eol eol, bool bom) {
    std::string data = bom ? "\xEF\xBB\xBF" : "";
    for (char c : content) { if (c == '\n' && eol == Eol::crlf) data += '\r'; data += c; }
    return data;
}

} // namespace detail
} // namespace dagent::workspace
