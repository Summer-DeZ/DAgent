#include "workspace/search.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <utility>

#include <unistd.h>

#include "base/text.hpp"
#include "exec/process.hpp"
#include "lib/nlohmann/json.hpp"

namespace dagent::workspace {
namespace fs = std::filesystem;
namespace {

using nlohmann::json;

fs::path find_rg(const SearchOptions& opt) {
    const auto missing = [](const std::string& detail) {
        throw WorkspaceError(WorkspaceError::Kind::tool_missing,
                             "ripgrep (rg) unavailable: " + detail +
                                 "; install it with: apt install ripgrep");
    };
    if (!opt.rg_path.empty()) {
        fs::path path = opt.rg_path;
        if (path.string().find('/') == std::string::npos) { // 命令名：在 PATH 里找
            const auto found = exec::which(path.string());
            if (!found) missing(path.string() + " not found in PATH");
            path = *found;
        }
        if (::access(path.c_str(), X_OK) != 0) missing(path.string() + " is not executable");
        return path;
    }
    static const fs::path cached = [] {
        const auto found = exec::which("rg");
        return found ? *found : fs::path{};
    }();
    if (cached.empty()) missing("not found in PATH");
    return cached;
}

// rg 的 --json 里 text 与 bytes 二选一，非 UTF-8 走 bytes（base64）。
std::string decode_field(const json& value) {
    if (value.contains("text")) return value.at("text").get<std::string>();
    if (value.contains("bytes")) {
        return base::to_valid_utf8(base::base64_decode(value.at("bytes").get<std::string>()));
    }
    return {};
}

std::string trim_newline(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

[[noreturn]] void rethrow_run(const exec::ExecError& e) {
    if (e.kind() == exec::ExecError::Kind::cancelled) {
        throw WorkspaceError(WorkspaceError::Kind::cancelled, "search cancelled");
    }
    throw WorkspaceError(WorkspaceError::Kind::io, std::format("ripgrep failed: {}", e.what()));
}

void check_exit(const exec::Result& result, std::string_view command) {
    if (!result.exit_code || *result.exit_code <= 1) return; // 0 有匹配、1 无匹配都正常
    const std::string detail = trim_newline(result.err.text);
    if (*result.exit_code == 2 && !detail.empty()) {
        throw WorkspaceError(WorkspaceError::Kind::bad_pattern, std::format("ripgrep: {}", detail));
    }
    throw WorkspaceError(WorkspaceError::Kind::io,
                         std::format("ripgrep {} exited with {}", command, *result.exit_code));
}

// ---------------------------------------------------------------- fzy 打分

double score_min() { return -std::numeric_limits<double>::max(); }

bool is_ascii_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_ascii_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }
char ascii_lower(char c) { return is_ascii_upper(c) ? static_cast<char>(c - 'A' + 'a') : c; }

// fzy 的 bonus 表：当前字符是大写时的奖励取决于前一字符，单独抽成函数便于阅读。
double bonus_for(char current, char last) {
    const int state = is_ascii_upper(current) ? 2 : (is_ascii_lower(current) || is_ascii_digit(current) ? 1 : 0);
    if (state == 0) return 0.0;
    if (last == '/') return 0.9;
    if (last == '-' || last == '_' || last == ' ') return 0.8;
    if (last == '.') return 0.6;
    if (state == 2 && is_ascii_lower(last)) return 0.7;
    return 0.0;
}

bool is_subsequence(std::string_view needle, std::string_view hay) {
    std::size_t j = 0;
    for (const char c : hay) {
        if (j < needle.size() && ascii_lower(c) == ascii_lower(needle[j])) ++j;
    }
    return j == needle.size();
}

// fzy 的 match()：D 是「以该位置匹配结尾」的最高分，M 是「到该位置为止」的最高分。
double fzy_score(std::string_view needle, std::string_view hay) {
    constexpr double kGapLeading = -0.005;
    constexpr double kGapTrailing = -0.005;
    constexpr double kGapInner = -0.01;
    constexpr double kMatchConsecutive = 1.0;

    const std::size_t n = needle.size();
    const std::size_t m = hay.size();
    const double kMin = score_min();
    if (n == 0 || m == 0 || n > m) return kMin;
    if (n == m) return 1.0; // 长度相同且能匹配上就是完全相等

    std::vector<double> bonus(m);
    char last = '/';
    for (std::size_t j = 0; j < m; ++j) {
        bonus[j] = bonus_for(hay[j], last);
        last = hay[j];
    }

    std::vector<double> last_d(m, kMin), last_m(m, kMin), curr_d(m, kMin), curr_m(m, kMin);
    for (std::size_t i = 0; i < n; ++i) {
        double prev_score = kMin;
        const double gap = (i == n - 1) ? kGapTrailing : kGapInner;
        double prev_d = kMin;
        double prev_m = kMin;
        for (std::size_t j = 0; j < m; ++j) {
            const bool hit = ascii_lower(needle[i]) == ascii_lower(hay[j]);
            double score = kMin;
            if (hit) {
                if (i == 0) {
                    score = static_cast<double>(j) * kGapLeading + bonus[j];
                } else if (j > 0) {
                    score = std::max(prev_m + bonus[j], prev_d + kMatchConsecutive);
                }
                prev_d = last_d[j];
                prev_m = last_m[j];
                curr_d[j] = score;
                curr_m[j] = prev_score = std::max(score, prev_score + gap);
            } else {
                prev_d = last_d[j];
                prev_m = last_m[j];
                curr_d[j] = kMin;
                curr_m[j] = prev_score = prev_score + gap;
            }
        }
        last_d.swap(curr_d);
        last_m.swap(curr_m);
    }
    return last_m[m - 1];
}

} // namespace

GrepResult grep(const GrepQuery& q, const SearchOptions& opt, std::stop_token stop) {
    const fs::path rg = find_rg(opt);

    std::vector<std::string> argv{rg.string(), "--json", "--no-messages", "--color=never",
                                  "--max-columns", "500", "--max-columns-preview"};
    if (q.fixed_strings) argv.emplace_back("--fixed-strings");
    argv.emplace_back(q.case_insensitive ? "--ignore-case" : "--smart-case");
    if (q.multiline) argv.emplace_back("--multiline");
    if (q.hidden) argv.emplace_back("--hidden");
    if (q.type) {
        argv.emplace_back("--type");
        argv.push_back(*q.type);
    }
    for (const std::string& glob : q.globs) {
        argv.emplace_back("--glob");
        argv.push_back(glob);
    }
    if (q.context > 0) {
        argv.emplace_back("--context");
        argv.push_back(std::to_string(q.context));
    }
    argv.emplace_back("--");
    argv.push_back(q.pattern);

    exec::Command cmd;
    // root 是普通文件时只搜这一个文件：cwd 换成它所在的目录，文件名作为 rg 的路径参数
    // （显式给出的路径 rg 不做忽略规则过滤，被 .gitignore 忽略的文件也能搜）。
    std::error_code ec;
    if (fs::is_regular_file(q.root, ec)) {
        argv.push_back(q.root.filename().string());
        cmd.cwd = q.root.parent_path();
    } else {
        cmd.cwd = q.root;
    }
    cmd.argv = std::move(argv);

    GrepResult result;
    std::string pending;
    std::size_t match_count = 0;
    std::stop_source inner;
    std::stop_callback relay(stop, [&] { inner.request_stop(); });

    const auto consume = [&](std::string_view line) {
        json event;
        try {
            event = json::parse(line);
        } catch (const json::exception& e) {
            throw WorkspaceError(WorkspaceError::Kind::io,
                                 std::format("invalid ripgrep JSON: {}", e.what()));
        }
        const std::string type = event.value("type", "");
        if (type == "match" || type == "context") {
            const json& data = event.at("data");
            Match m;
            m.path = decode_field(data.at("path"));
            m.line = data.at("line_number").get<std::uint64_t>();
            m.text = trim_newline(decode_field(data.at("lines")));
            m.is_context = type == "context";
            if (!m.is_context) {
                for (const json& span : data.at("submatches")) {
                    m.spans.emplace_back(span.at("start").get<std::size_t>(),
                                         span.at("end").get<std::size_t>());
                }
                ++match_count;
            }
            result.matches.push_back(std::move(m));
            if (match_count >= q.max_matches) {
                result.truncated = true;
                inner.request_stop();
            }
        } else if (type == "end") {
            if (event.at("data").at("stats").value("matches", 0) > 0) ++result.files_with_matches;
        }
    };

    const auto on_output = [&](exec::Stream stream, std::string_view chunk) {
        if (stream != exec::Stream::out || result.truncated) return;
        pending.append(chunk);
        for (std::size_t pos = 0; (pos = pending.find('\n')) != std::string::npos;) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty()) consume(line);
            if (result.truncated) break; // 到达上限，丢弃同一批里剩下的行
        }
    };

    exec::Result outcome;
    try {
        outcome = exec::run(cmd, {}, on_output, inner.get_token());
    } catch (const exec::ExecError& e) {
        if (e.kind() == exec::ExecError::Kind::cancelled && !stop.stop_requested()) return result;
        rethrow_run(e);
    }
    if (!result.truncated) check_exit(outcome, "grep");
    return result;
}

std::vector<std::string> files(const FilesQuery& q, const SearchOptions& opt, std::stop_token stop) {
    const fs::path rg = find_rg(opt);

    std::vector<std::string> argv{rg.string(), "--files", "--null", "--no-messages", "--color=never"};
    if (q.hidden) argv.emplace_back("--hidden");
    for (const std::string& glob : q.globs) {
        argv.emplace_back("--glob");
        argv.push_back(glob);
    }

    exec::Command cmd;
    cmd.argv = std::move(argv);
    cmd.cwd = q.root;

    std::vector<std::string> found;
    bool stopped_early = false;
    std::string pending;
    std::stop_source inner;
    std::stop_callback relay(stop, [&] { inner.request_stop(); });

    const auto on_output = [&](exec::Stream stream, std::string_view chunk) {
        if (stream != exec::Stream::out || stopped_early) return;
        pending.append(chunk);
        for (std::size_t pos = 0; (pos = pending.find('\0')) != std::string::npos;) {
            std::string path = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (path.empty()) continue;
            found.push_back(std::move(path));
            if (!q.sort_by_mtime && found.size() >= q.max_files) {
                stopped_early = true;
                inner.request_stop();
                return;
            }
        }
    };

    exec::Result outcome;
    try {
        outcome = exec::run(cmd, {}, on_output, inner.get_token());
    } catch (const exec::ExecError& e) {
        if (e.kind() == exec::ExecError::Kind::cancelled && !stop.stop_requested()) {
            // 数量够了主动停掉 rg，按正常结果返回
        } else {
            rethrow_run(e);
        }
    }
    if (!stopped_early) check_exit(outcome, "files");

    if (!q.sort_by_mtime) {
        if (found.size() > q.max_files) found.resize(q.max_files);
        return found;
    }

    // 不按 mtime 排序时可以在够数后提前停 rg；排 mtime 只能拿全量再自己 stat（rg --sort 会退化成单线程）。
    std::vector<std::pair<fs::file_time_type, std::string>> stamped;
    stamped.reserve(found.size());
    for (std::string& path : found) {
        std::error_code ec;
        const auto mtime = fs::last_write_time(q.root / path, ec);
        stamped.emplace_back(ec ? fs::file_time_type::min() : mtime, std::move(path));
    }
    std::stable_sort(stamped.begin(), stamped.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    if (stamped.size() > q.max_files) stamped.resize(q.max_files);

    std::vector<std::string> sorted;
    sorted.reserve(stamped.size());
    for (auto& [mtime, path] : stamped) sorted.push_back(std::move(path));
    return sorted;
}

std::vector<std::size_t> fuzzy_rank(std::string_view needle, std::span<const std::string> haystack,
                                    std::size_t limit) {
    std::vector<std::size_t> ranked;
    if (limit == 0) return ranked;

    std::vector<std::pair<double, std::size_t>> scored;
    scored.reserve(haystack.size());
    for (std::size_t i = 0; i < haystack.size(); ++i) {
        if (!is_subsequence(needle, haystack[i])) continue;
        scored.emplace_back(fzy_score(needle, haystack[i]), i);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });

    const std::size_t count = std::min(limit, scored.size());
    ranked.reserve(count);
    for (std::size_t i = 0; i < count; ++i) ranked.push_back(scored[i].second);
    return ranked;
}

} // namespace dagent::workspace
