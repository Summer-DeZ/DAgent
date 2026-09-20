#include "exec/shell.hpp"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" const TSLanguage* tree_sitter_bash(void);

namespace dagent::exec {
namespace {

using Node = TSNode;

// 未加引号的 word：反斜杠转义下一个字符，反斜杠换行是续行。
std::string decode_word(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            if (text[i + 1] != '\n') out += text[i + 1];
            ++i;
            continue;
        }
        out += c;
    }
    return out;
}

// 未加引号的 word 里有未转义的 '{'：bash 可能做花括号展开（-{delete,print} → -delete -print），
// 字面量不再等于实际参数，按动态处理。
bool has_unescaped_brace(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\') {
            ++i;
            continue;
        }
        if (text[i] == '{') return true;
    }
    return false;
}

// 双引号内部：反斜杠只对 $ ` " \ 和换行有特殊含义，其余原样保留。
std::string decode_double_quoted(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            const char next = text[i + 1];
            if (next == '$' || next == '`' || next == '"' || next == '\\') {
                out += next;
                ++i;
                continue;
            }
            if (next == '\n') {
                ++i;
                continue;
            }
            out += c;
            continue;
        }
        out += c;
    }
    return out;
}

class Analyzer {
public:
    explicit Analyzer(std::string_view source) : source_(source) {}

    void analyze(Node root) {
        if (ts_node_has_error(root)) opaque_ = true;
        visit(root);
    }

    void mark_opaque() { opaque_ = true; }

    Analysis take() { return {std::move(commands_), opaque_}; }

private:
    std::string_view text_of(Node node) const {
        const std::uint32_t begin = ts_node_start_byte(node);
        return source_.substr(begin, ts_node_end_byte(node) - begin);
    }

    // 双引号串里只要出现 string_content 之外的命名子节点，就含有动态展开。
    bool string_is_dynamic(Node node) const {
        const std::uint32_t count = ts_node_named_child_count(node);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (ts_node_type(ts_node_named_child(node, i)) != std::string_view("string_content")) return true;
        }
        return false;
    }

    // 把字面量还原成 argv 里的字符串；含动态展开或未知结构时返回 nullopt。
    std::optional<std::string> literal(Node node) const {
        const std::string_view type = ts_node_type(node);
        const std::string_view text = text_of(node);

        if (type == "word") {
            if (has_unescaped_brace(text)) return std::nullopt;
            return decode_word(text);
        }
        if (type == "number") return std::string(text);
        if (type == "command_name") {
            if (ts_node_named_child_count(node) == 1) return literal(ts_node_named_child(node, 0));
            return decode_word(text);
        }
        if (type == "raw_string") {
            if (text.size() < 2) return std::nullopt;
            return std::string(text.substr(1, text.size() - 2));
        }
        if (type == "string") {
            if (string_is_dynamic(node) || text.size() < 2) return std::nullopt;
            return decode_double_quoted(text.substr(1, text.size() - 2));
        }
        if (type == "concatenation") {
            std::string result;
            const std::uint32_t count = ts_node_named_child_count(node);
            for (std::uint32_t i = 0; i < count; ++i) {
                auto part = literal(ts_node_named_child(node, i));
                if (!part) return std::nullopt;
                result += *part;
            }
            return result;
        }
        return std::nullopt;
    }

    void visit_children(Node node) {
        const std::uint32_t count = ts_node_named_child_count(node);
        for (std::uint32_t i = 0; i < count; ++i) visit(ts_node_named_child(node, i));
    }

    void visit(Node node) {
        const std::string_view type = ts_node_type(node);

        // 纯字面或数据节点：既没有命令，也不会把命令藏起来。
        if (type == "comment" || type == "string_content" || type == "raw_string" || type == "word" ||
            type == "number" || type == "variable_name" || type == "special_variable_name" ||
            type == "file_descriptor" || type == "heredoc_start" || type == "heredoc_end" ||
            type == "heredoc_body" || type == "simple_heredoc_body" || type == "ansi_c_string") {
            return;
        }

        if (type == "string") {
            if (string_is_dynamic(node)) {
                opaque_ = true;
                visit_children(node);
            }
            return;
        }

        if (type == "command") {
            extract(node);
            return;
        }

        // 纯结构节点：只继续往下收集。
        if (type == "program" || type == "list" || type == "pipeline" || type == "negated_command" ||
            type == "concatenation" ||
            type == "do_group" || type == "elif_clause" || type == "else_clause" || type == "condition" ||
            type == "consequence" || type == "body" || type == "case_item" || type == "last_case_item") {
            visit_children(node);
            return;
        }

        // 变量赋值（LD_PRELOAD=x cat、PATH=/tmp; ls 都能让「只读」命令执行任意代码）、展开、控制流、
        // heredoc 重定向和未知识别：无法静态判断，按不透明处理并继续收集嵌套命令。
        opaque_ = true;
        visit_children(node);
    }

    void extract(Node command) {
        SimpleCommand simple;
        bool name_resolved = false;
        const std::uint32_t count = ts_node_named_child_count(command);
        for (std::uint32_t i = 0; i < count; ++i) {
            Node child = ts_node_named_child(command, i);
            const std::string_view type = ts_node_type(child);

            if (type == "command_name") {
                auto value = literal(child);
                if (value) {
                    simple.argv.push_back(std::move(*value));
                    name_resolved = true;
                } else {
                    opaque_ = true;
                    visit(child);
                }
                continue;
            }
            if (type == "file_redirect" || type == "heredoc_redirect" || type == "herestring_redirect") {
                // 写文件的重定向、heredoc 正文里的展开都无法静态判断；正文是数据，不再深入。
                opaque_ = true;
                continue;
            }
            if (type == "file_descriptor") continue;
            if (type == "variable_assignment" || type == "variable_assignments") {
                visit(child);  // visit 会标记 opaque，并收集赋值值里嵌套的命令
                continue;
            }
            if (type == "subshell" || type == "compound_statement") {
                opaque_ = true;
                visit(child);
                continue;
            }

            auto value = literal(child);
            if (value) {
                simple.argv.push_back(std::move(*value));
                continue;
            }
            opaque_ = true;
            visit(child);
        }
        // 命令名自己就是动态展开时，argv 不可信，不放进 commands。
        if (name_resolved) commands_.push_back(std::move(simple));
    }

    std::string_view source_;
    std::vector<SimpleCommand> commands_;
    bool opaque_ = false;
};

bool find_is_readonly(const std::vector<std::string>& argv) {
    // find 默认只遍历并打印；下面这些动作会执行命令或写文件。
    static constexpr std::string_view kActions[] = {"-exec", "-execdir", "-ok",  "-okdir",    "-delete",
                                                    "-fls",  "-fprint",  "-fprint0", "-fprintf"};
    for (std::size_t i = 1; i < argv.size(); ++i) {
        for (const auto action : kActions) {
            if (argv[i] == action) return false;
        }
    }
    return true;
}

bool rg_is_readonly(const std::vector<std::string>& argv) {
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--") break;  // 之后都是模式和路径
        if (arg.rfind("--pre", 0) == 0 || arg.rfind("--hostname-bin", 0) == 0 || arg == "--search-zip")
            return false;
        // 短选项可以合写（-iz）；只看选项字母，-e/-f/-g 等带值的选项后面的字母是值，保守起见一并拒绝
        if (arg.size() > 1 && arg[0] == '-' && arg[1] != '-' && arg.find('z') != std::string::npos)
            return false;
    }
    return true;
}

// git branch 只放行列出分支的形式：带位置参数（创建）或 -d/-D/-m/-c/-u 等都会写仓库。
bool git_branch_is_readonly(const std::vector<std::string>& argv, std::size_t index) {
    static constexpr std::string_view kFlags[] = {
        "-a", "--all", "-r", "--remotes", "-v", "-vv", "--verbose", "-l", "--list", "--show-current",
        "--no-color", "--color", "--no-column", "--column", "-i", "--ignore-case", "--omit-empty"};
    // 这些选项的值可以写成下一个参数（--contains HEAD）
    static constexpr std::string_view kValued[] = {"--contains", "--no-contains", "--merged",
                                                   "--no-merged", "--points-at", "--sort", "--format"};
    bool list_mode = false;
    bool has_pattern = false;
    for (++index; index < argv.size(); ++index) {
        const std::string_view arg = argv[index];
        if (arg == "-l" || arg == "--list") list_mode = true;
        bool known = false;
        for (const auto flag : kFlags) known = known || arg == flag || arg.starts_with(std::string(flag) + "=");
        for (const auto flag : kValued) {
            if (arg == flag) {
                known = true;
                ++index;  // 跳过值
            } else if (arg.starts_with(std::string(flag) + "=")) {
                known = true;
            }
        }
        if (known) continue;
        if (!arg.empty() && arg[0] == '-') return false;  // 其余选项（-d/-D/-m/-M/-c/-C/-u/-f/--edit-description…）
        has_pattern = true;                               // 位置参数：只有 --list 下才是匹配模式
    }
    return !has_pattern || list_mode;
}

bool git_is_readonly(const std::vector<std::string>& argv) {
    std::size_t index = 1;
    for (; index < argv.size(); ++index) {
        const std::string& arg = argv[index];
        if (arg.empty() || arg[0] != '-') break;
        if (arg == "--no-pager" || arg == "-P") continue;
        // -c/--config-env 可以注入 alias 执行任意命令；--exec-path 能换掉外部程序目录。
        if (arg == "-c" || arg.rfind("-c", 0) == 0) return false;
        if (arg.rfind("--config-env", 0) == 0 || arg.rfind("--exec-path", 0) == 0) return false;
        return false;  // 其余全局选项保守拒绝
    }
    if (index >= argv.size()) return false;

    const std::string& subcommand = argv[index];
    if (subcommand != "status" && subcommand != "log" && subcommand != "diff" && subcommand != "show" &&
        subcommand != "branch") {
        return false;
    }

    if (subcommand == "branch") return git_branch_is_readonly(argv, index);

    for (++index; index < argv.size(); ++index) {
        const std::string& arg = argv[index];
        if (arg.rfind("--output", 0) == 0) return false;               // 可以把结果写进文件
        if (arg == "--ext-diff" || arg == "--textconv") return false;  // 会调外部程序
        if (arg.rfind("--open-files-in-pager", 0) == 0) return false;  // 会起分页器
    }
    return true;
}

bool is_readonly_command(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;
    const std::string& name = argv[0];
    if (name.find('/') != std::string::npos) return false;  // 路径可能指向任意程序

    // ls/cat/head/tail/wc：只读取文件或标准输入并输出。
    if (name == "ls" || name == "cat" || name == "head" || name == "tail" || name == "wc") return true;
    // rg：只在输入里搜索文本；--pre/-z/--hostname-bin 会执行外部程序，由 rg_is_readonly 拒绝。
    if (name == "rg") return rg_is_readonly(argv);
    // grep：只在输入里搜索文本，没有执行外部程序或写文件的选项。
    if (name == "grep") return true;
    // find：默认只读；-exec/-delete/-fprint* 等动作由 find_is_readonly 拒绝。
    if (name == "find") return find_is_readonly(argv);
    // git：只放行纯查看子命令（branch 只放行列出形式），且拒绝能注入命令或写文件的选项。
    // 注意仓库自己的 .git/config（core.fsmonitor、diff.external 等）仍可能让这些命令执行程序，
    // 所以自动放行的命令仍应在 read_only 沙箱里执行。
    if (name == "git") return git_is_readonly(argv);
    // pwd/echo：只往标准输出写内容。
    if (name == "pwd" || name == "echo") return true;
    return false;
}

} // namespace

Analysis analyze(std::string_view bash_source) {
    TSParser* parser = ts_parser_new();
    if (!ts_parser_set_language(parser, tree_sitter_bash())) {
        ts_parser_delete(parser);
        Analysis analysis;
        analysis.has_opaque = true;
        return analysis;
    }

    TSTree* tree =
        ts_parser_parse_string(parser, nullptr, bash_source.data(), static_cast<std::uint32_t>(bash_source.size()));
    Analyzer analyzer(bash_source);
    if (tree == nullptr) {
        analyzer.mark_opaque();
    } else {
        analyzer.analyze(ts_tree_root_node(tree));
        ts_tree_delete(tree);
    }
    ts_parser_delete(parser);
    return analyzer.take();
}

bool is_known_readonly(const Analysis& analysis) {
    if (analysis.has_opaque || analysis.commands.empty()) return false;
    for (const auto& command : analysis.commands) {
        if (!is_readonly_command(command.argv)) return false;
    }
    return true;
}

bool is_dangerous(std::string_view source) {
    const Analysis analysis = analyze(source);
    const auto basename = [](std::string_view value) {
        const auto slash = value.rfind('/');
        return slash == std::string_view::npos ? value : value.substr(slash + 1);
    };
    const auto recursive_force = [](const std::vector<std::string>& argv) {
        bool recursive = false, force = false;
        for (std::size_t i = 1; i < argv.size(); ++i) {
            const std::string_view arg = argv[i];
            recursive = recursive || arg == "--recursive" ||
                        (arg.starts_with('-') && !arg.starts_with("--") && arg.find('r') != std::string_view::npos);
            force = force || arg == "--force" ||
                    (arg.starts_with('-') && !arg.starts_with("--") && arg.find('f') != std::string_view::npos);
        }
        return recursive && force;
    };
    const char* home = std::getenv("HOME");
    const auto broad_target = [home](std::string_view raw) {
        if (raw == "/" || raw == "~" || raw == "$HOME" || raw == "${HOME}") return true;
        if (home != nullptr && raw == home) return true;
        if (!raw.starts_with('/')) return false;
        std::filesystem::path path(raw);
        return static_cast<int>(std::distance(path.begin(), path.end())) <= 2;
    };
    const auto device = [](std::string_view value) {
        return value.starts_with("/dev/sd") || value.starts_with("/dev/nvme") ||
               value.starts_with("/dev/vd") || value.starts_with("/dev/mmcblk");
    };
    const bool raw_device = source.find("/dev/sd") != std::string_view::npos ||
                            source.find("/dev/nvme") != std::string_view::npos ||
                            source.find("/dev/vd") != std::string_view::npos ||
                            source.find("/dev/mmcblk") != std::string_view::npos;
    if (raw_device && source.find('>') != std::string_view::npos) return true;

    bool downloader = false, shell = false;
    for (const SimpleCommand& command : analysis.commands) {
        if (command.argv.empty()) continue;
        const std::string_view name = basename(command.argv.front());
        if (name.starts_with("mkfs")) return true;
        if (name == "shutdown" || name == "reboot" || name == "halt" || name == "poweroff") return true;
        downloader = downloader || name == "curl" || name == "wget";
        shell = shell || name == "sh" || name == "bash";

        if (name == "dd") {
            for (const std::string& arg : command.argv)
                if (arg.starts_with("of=") && device(std::string_view(arg).substr(3))) return true;
        }
        if ((name == "tee" || name == "cp" || name == "mv" || name == "install") &&
            std::ranges::any_of(command.argv, [&](const std::string& arg) { return device(arg); })) return true;
        if (name == "rm" && recursive_force(command.argv)) {
            if (source.find("$HOME") != std::string_view::npos ||
                source.find("${HOME}") != std::string_view::npos) return true;
            for (std::size_t i = 1; i < command.argv.size(); ++i)
                if (!command.argv[i].starts_with('-') && broad_target(command.argv[i])) return true;
        }
        if ((name == "chmod" || name == "chown") &&
            std::ranges::any_of(command.argv, [](const std::string& arg) {
                return arg == "-R" || arg == "--recursive";
            })) {
            for (std::size_t i = 1; i < command.argv.size(); ++i)
                if (!command.argv[i].starts_with('-') && broad_target(command.argv[i])) return true;
        }
    }
    return downloader && shell && source.find('|') != std::string_view::npos;
}

} // namespace dagent::exec
