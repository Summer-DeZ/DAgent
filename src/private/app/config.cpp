#include "app/config.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

#include "base/log.hpp"
#include "exec/process.hpp"

namespace dagent::app {
namespace fs = std::filesystem;
namespace {

using json = nlohmann::json;

std::shared_ptr<spdlog::logger> log_app() { return base::logger("app"); }

[[noreturn]] void fail(ConfigError::Kind kind, const std::string& what) {
    throw ConfigError(kind, what);
}

std::string trim_end(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

fs::path absolute_under(const fs::path& base, const fs::path& path) {
    std::error_code ec;
    fs::path result = fs::weakly_canonical(base / path, ec);
    if (!ec) return result;
    ec.clear();
    result = fs::absolute(base / path, ec);
    return ec ? (base / path).lexically_normal() : result.lexically_normal();
}

/// @brief 带 JSON 指针的强类型读取；类型不符时抛 ConfigError{type}。
class Node {
public:
    Node() = default;
    Node(const json& value, std::string pointer) : value_(&value), pointer_(std::move(pointer)) {}

    bool exists() const { return value_ != nullptr; }
    bool has() const { return value_ != nullptr && !value_->is_null(); }

    Node child(std::string_view key) const {
        if (!exists()) return {};
        if (!value_->is_object()) wrong("对象");
        const auto it = value_->find(key);
        if (it == value_->end()) return {};
        return Node(*it, pointer_ + "/" + std::string(key));
    }

    const json& raw() const { return *value_; }
    const std::string& pointer() const { return pointer_; }

    std::string str(std::string fallback = {}) const {
        if (!has()) return fallback;
        if (!value_->is_string()) wrong("字符串");
        return value_->get<std::string>();
    }

    bool flag(bool fallback = false) const {
        if (!has()) return fallback;
        if (!value_->is_boolean()) wrong("布尔值");
        return value_->get<bool>();
    }

    int integer(int fallback = 0) const {
        if (!has()) return fallback;
        if (value_->is_number_unsigned()) {
            const auto n = value_->get<std::uint64_t>();
            if (n > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) wrong("32 位整数");
            return static_cast<int>(n);
        }
        if (value_->is_number_integer()) {
            const auto n = value_->get<std::int64_t>();
            if (n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max())
                wrong("32 位整数");
            return static_cast<int>(n);
        }
        wrong("整数");
    }

    std::size_t usize(std::size_t fallback = 0) const {
        if (!has()) return fallback;
        if (value_->is_number_unsigned()) return value_->get<std::size_t>();
        if (value_->is_number_integer()) {
            const auto n = value_->get<std::int64_t>();
            if (n >= 0) return static_cast<std::size_t>(n);
        }
        wrong("非负整数");
    }

    double real(double fallback = 0) const {
        if (!has()) return fallback;
        if (!value_->is_number()) wrong("数字");
        return value_->get<double>();
    }

    std::vector<std::string> strings() const {
        if (!has()) return {};
        if (!value_->is_array()) wrong("字符串数组");
        std::vector<std::string> out;
        out.reserve(value_->size());
        for (const auto& item : *value_) {
            if (!item.is_string()) wrong("字符串数组");
            out.push_back(item.get<std::string>());
        }
        return out;
    }

private:
    [[noreturn]] void wrong(std::string_view expected) const {
        fail(ConfigError::Kind::type, std::format("{} 应为{}", pointer_, expected));
    }

    const json* value_ = nullptr;
    std::string pointer_;
};

json parse_layer(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) fail(ConfigError::Kind::io, "打不开配置文件：" + file.string());
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    json layer;
    try {
        layer = json::parse(text, nullptr, true, /*ignore_comments=*/true);
    } catch (const json::parse_error& e) {
        fail(ConfigError::Kind::parse, file.string() + "：" + e.what());
    }
    if (!layer.is_object()) fail(ConfigError::Kind::parse, file.string() + "：顶层应为 JSON 对象");
    return layer;
}

/// @brief 把这一层里的路径字段先解析成绝对路径（相对它所在的文件），再参与合并。
void resolve_path_fields(json& layer, const fs::path& base) {
    // command_like：不含 '/' 的值是命令名（在 PATH 里找），不是相对路径。
    const auto resolve = [&](std::initializer_list<std::string_view> keys, bool command_like = false) {
        json* node = &layer;
        for (const std::string_view key : keys) {
            if (!node->is_object()) return;
            const auto it = node->find(std::string(key));
            if (it == node->end()) return;
            node = &*it;
        }
        if (!node->is_string()) return;
        const std::string text = node->get<std::string>();
        if (command_like && text.find('/') == std::string::npos) return;
        const fs::path value = text;
        if (value.empty() || value.is_absolute()) return;
        *node = absolute_under(base, value).string();
    };
    resolve({"gateway", "system_prompt_file"});
    resolve({"session", "directory"});
    resolve({"search", "rg_path"}, /*command_like=*/true);
    resolve({"log", "file"});
}

const std::set<std::string>& known_keys() {
    static const std::set<std::string> keys = {
        "gateway.base_url", "gateway.model", "gateway.max_tokens", "gateway.temperature",
        "gateway.enable_thinking", "gateway.system_prompt_file", "gateway.api_key_env",
        "http.timeout_seconds", "http.connect_timeout_seconds", "http.idle_timeout_seconds",
        "http.max_body_bytes", "http.max_error_body_bytes", "http.verify_peer", "http.verify_host",
        "context.window_tokens", "context.safety_margin_tokens", "context.compaction_trigger_percent",
        "context.compaction_target_percent",
        "files.max_read_bytes", "files.max_write_bytes",
        "search.rg_path",
        "process.default_timeout_ms", "process.max_output_bytes", "process.kill_grace_ms",
        "process.drain_after_exit_ms", "process.env_deny",
        "run.max_model_calls", "run.max_tool_calls", "run.max_model_retries",
        "session.directory", "session.record_payloads", "session.max_inline_payload_bytes",
        "session.redact_fields",
        "log.file", "log.max_file_bytes", "log.max_files", "log.level", "log.also_stderr",
        "progress.interval_ms", "permissions",
        "mcp.connect_timeout_ms", "mcp.probe_timeout_ms",
        "tools.max_result_bytes", "tools.read_default_lines", "tools.read_max_line_bytes",
        "tools.grep_max_matches", "tools.glob_max_files", "tools.bash_max_timeout_ms",
        "tools.mcp_call_timeout_ms",
    };
    return keys;
}

bool known_key(std::string_view key) {
    if (known_keys().contains(std::string(key))) return true;
    // flatten() 会把数组拆成 key.0、key.1；空对象则原样保留。
    static constexpr std::string_view kArrayKeys[] = {"session.redact_fields", "process.env_deny"};
    for (const std::string_view array_key : kArrayKeys) {
        if (key == array_key || (key.size() > array_key.size() && key.starts_with(array_key) &&
                                 key[array_key.size()] == '.'))
            return true;
    }
    constexpr std::string_view kCredentials = "network.credentials";
    return key == kCredentials || (key.size() > kCredentials.size() && key.starts_with(kCredentials) &&
                                   key[kCredentials.size()] == '.');
}

void warn_unknown(const json& merged) {
    const json flat = merged.flatten(); // 键是 /a/b 形式的 JSON 指针
    for (const auto& [pointer, value] : flat.items()) {
        (void)value;
        std::string key = pointer;
        for (char& c : key)
            if (c == '/') c = '.';
        if (!key.empty() && key.front() == '.') key.erase(0, 1);
        if (known_key(key)) continue;
        log_app()->warn("未知配置项 {}", key);
    }
}

json build_override_layer(const std::vector<std::string>& overrides, const fs::path& base) {
    json layer = json::object();
    for (const auto& entry : overrides) {
        const auto eq = entry.find('=');
        if (eq == std::string::npos || eq == 0)
            fail(ConfigError::Kind::type, "无效的配置覆盖（应为 键=值）：" + entry);
        const std::string key = entry.substr(0, eq);
        const std::string raw = entry.substr(eq + 1);
        json value = json::parse(raw, nullptr, false);
        if (value.is_discarded()) value = raw; // 不是合法 JSON 就当字符串

        std::vector<std::string> parts;
        std::string_view rest = key;
        while (true) {
            const auto dot = rest.rfind('.');
            parts.push_back(std::string(rest.substr(dot + 1)));
            if (dot == std::string_view::npos) break;
            rest = rest.substr(0, dot);
        }
        json node = std::move(value);
        for (const auto& part : parts) node = json{{part, node}};
        layer.merge_patch(node);
    }
    resolve_path_fields(layer, base);
    return layer;
}

fs::path user_config_dir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') return xdg;
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        return fs::path(home) / ".config";
    return {};
}

Gateway map_gateway(const Node& node, const base::Secrets& secrets) {
    Gateway gateway;
    if (const Node v = node.child("base_url"); v.has()) gateway.base_url = v.str();
    if (const Node v = node.child("model"); v.has()) gateway.model = v.str();
    if (const Node v = node.child("max_tokens"); v.has()) gateway.max_tokens = v.integer(gateway.max_tokens);
    if (const Node v = node.child("temperature"); v.has()) gateway.temperature = v.real();
    if (const Node v = node.child("enable_thinking"); v.has()) gateway.enable_thinking = v.flag();
    if (const Node v = node.child("system_prompt_file"); v.has())
        gateway.system_prompt_file = v.str();
    if (const Node v = node.child("api_key_env"); v.has()) {
        const std::string name = v.str();
        if (!name.empty()) {
            if (const auto value = secrets.get(name)) gateway.api_key = *value;
            else log_app()->warn("gateway.api_key_env 指向的变量 {} 没有值", name);
        }
    }
    return gateway;
}

net::HttpOptions map_http(const Node& node) {
    net::HttpOptions opt;
    if (const Node v = node.child("timeout_seconds"); v.has())
        opt.timeout = std::chrono::seconds{v.integer()};
    if (const Node v = node.child("connect_timeout_seconds"); v.has())
        opt.connect_timeout = std::chrono::seconds{v.integer()};
    if (const Node v = node.child("idle_timeout_seconds"); v.has())
        opt.idle_timeout = std::chrono::seconds{v.integer()};
    if (const Node v = node.child("max_body_bytes"); v.has()) opt.max_body_bytes = v.usize(opt.max_body_bytes);
    if (const Node v = node.child("max_error_body_bytes"); v.has())
        opt.max_error_body_bytes = v.usize(opt.max_error_body_bytes);
    if (const Node v = node.child("verify_peer"); v.has()) opt.verify_peer = v.flag(opt.verify_peer);
    if (const Node v = node.child("verify_host"); v.has()) opt.verify_host = v.flag(opt.verify_host);
    return opt;
}

exec::Options map_process(const Node& node) {
    exec::Options opt;
    if (const Node v = node.child("default_timeout_ms"); v.has())
        opt.default_timeout = std::chrono::milliseconds{v.integer()};
    if (const Node v = node.child("max_output_bytes"); v.has())
        opt.max_output_bytes = v.usize(opt.max_output_bytes);
    if (const Node v = node.child("kill_grace_ms"); v.has())
        opt.kill_grace = std::chrono::milliseconds{v.integer()};
    if (const Node v = node.child("drain_after_exit_ms"); v.has())
        opt.drain_after_exit = std::chrono::milliseconds{v.integer()};
    if (const Node v = node.child("env_deny"); v.has()) opt.env_deny = v.strings();
    return opt;
}

workspace::FileOptions map_files(const Node& node) {
    workspace::FileOptions opt;
    if (const Node v = node.child("max_read_bytes"); v.has()) opt.max_read_bytes = v.usize(opt.max_read_bytes);
    if (const Node v = node.child("max_write_bytes"); v.has())
        opt.max_write_bytes = v.usize(opt.max_write_bytes);
    return opt;
}

workspace::SearchOptions map_search(const Node& node) {
    workspace::SearchOptions opt;
    if (const Node v = node.child("rg_path"); v.has()) opt.rg_path = v.str();
    return opt;
}

session::Options map_session(const Node& node) {
    session::Options opt;
    if (const Node v = node.child("directory"); v.has()) opt.directory = v.str();
    if (const Node v = node.child("record_payloads"); v.has())
        opt.record_payloads = v.flag(opt.record_payloads);
    if (const Node v = node.child("max_inline_payload_bytes"); v.has())
        opt.max_inline_payload_bytes = v.usize(opt.max_inline_payload_bytes);
    if (const Node v = node.child("redact_fields"); v.has()) opt.redact_fields = v.strings();
    return opt;
}

base::LogOptions map_log(const Node& node) {
    base::LogOptions opt;
    if (const Node v = node.child("file"); v.has()) opt.file = v.str();
    if (const Node v = node.child("max_file_bytes"); v.has()) opt.max_file_bytes = v.usize(opt.max_file_bytes);
    if (const Node v = node.child("max_files"); v.has()) opt.max_files = v.usize(opt.max_files);
    if (const Node v = node.child("level"); v.has()) opt.level = v.str(opt.level);
    if (const Node v = node.child("also_stderr"); v.has()) opt.also_stderr = v.flag(opt.also_stderr);
    return opt;
}

ContextOptions map_context(const Node& node) {
    ContextOptions opt;
    if (const Node v = node.child("window_tokens"); v.has()) opt.window_tokens = v.integer(opt.window_tokens);
    if (const Node v = node.child("safety_margin_tokens"); v.has())
        opt.safety_margin_tokens = v.integer(opt.safety_margin_tokens);
    if (const Node v = node.child("compaction_trigger_percent"); v.has())
        opt.compaction_trigger_percent = v.integer(opt.compaction_trigger_percent);
    if (const Node v = node.child("compaction_target_percent"); v.has())
        opt.compaction_target_percent = v.integer(opt.compaction_target_percent);
    return opt;
}

RunOptions map_run(const Node& node) {
    RunOptions opt;
    if (const Node v = node.child("max_model_calls"); v.has())
        opt.max_model_calls = v.integer(opt.max_model_calls);
    if (const Node v = node.child("max_tool_calls"); v.has())
        opt.max_tool_calls = v.integer(opt.max_tool_calls);
    if (const Node v = node.child("max_model_retries"); v.has())
        opt.max_model_retries = v.integer(opt.max_model_retries);
    return opt;
}

ProgressOptions map_progress(const Node& node) {
    ProgressOptions opt;
    if (const Node v = node.child("interval_ms"); v.has())
        opt.interval = std::chrono::milliseconds{v.integer()};
    return opt;
}

mcp::Options map_mcp(const Node& node) {
    mcp::Options opt;
    if (const Node v = node.child("connect_timeout_ms"); v.has())
        opt.connect_timeout = std::chrono::milliseconds{v.integer()};
    if (const Node v = node.child("probe_timeout_ms"); v.has())
        opt.probe_timeout = std::chrono::milliseconds{v.integer()};
    return opt;
}

tools::Options map_tools(const Node& node) {
    tools::Options opt;
    if (const Node v = node.child("max_result_bytes"); v.has()) opt.max_result_bytes = v.usize(opt.max_result_bytes);
    if (const Node v = node.child("read_default_lines"); v.has())
        opt.read_default_lines = v.integer(opt.read_default_lines);
    if (const Node v = node.child("read_max_line_bytes"); v.has())
        opt.read_max_line_bytes = v.usize(opt.read_max_line_bytes);
    if (const Node v = node.child("grep_max_matches"); v.has()) opt.grep_max_matches = v.usize(opt.grep_max_matches);
    if (const Node v = node.child("glob_max_files"); v.has()) opt.glob_max_files = v.usize(opt.glob_max_files);
    if (const Node v = node.child("bash_max_timeout_ms"); v.has())
        opt.bash_max_timeout = std::chrono::milliseconds{v.integer()};
    if (const Node v = node.child("mcp_call_timeout_ms"); v.has())
        opt.mcp_call_timeout = std::chrono::milliseconds{v.integer()};
    return opt;
}

std::map<std::string, std::string> map_credentials(const Node& node, const base::Secrets& secrets) {
    std::map<std::string, std::string> out;
    const Node credentials = node.child("credentials");
    if (!credentials.has()) return out;
    if (!credentials.raw().is_object())
        fail(ConfigError::Kind::type, credentials.pointer() + " 应为对象");
    for (const auto& [host, value] : credentials.raw().items()) {
        if (!value.is_string())
            fail(ConfigError::Kind::type, credentials.pointer() + "/" + host + " 应为字符串（变量名）");
        const std::string name = value.get<std::string>();
        if (const auto secret = secrets.get(name)) out[host] = *secret;
        else log_app()->warn("network.credentials.{} 指向的变量 {} 没有值", host, name);
    }
    return out;
}

// 变量不存在时报错而不是替换成空串：空的 token/参数会在很久以后变成难以理解的鉴权失败。
// 报错信息只带变量名和位置，不带任何展开后的值。
std::string expand_placeholders(const std::string& text, const base::Secrets& secrets, const std::string& where) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '{') {
            const auto close = text.find('}', i + 2);
            if (close != std::string::npos) {
                const std::string name = text.substr(i + 2, close - i - 2);
                const auto value = secrets.get(name);
                if (!value)
                    fail(ConfigError::Kind::invalid,
                         std::format("{} 引用的变量 {} 没有值（环境变量与 .env 里都找不到）", where, name));
                out += *value;
                i = close + 1;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

std::vector<mcp::ServerConfig> load_mcp_servers(const fs::path& root, bool trusted, const base::Secrets& secrets,
                                                std::vector<fs::path>& untrusted) {
    json servers = json::object();
    // 同名 server 整条替换，不逐字段合并（否则会拼出一半 http 一半 stdio 的条目）；null 表示去掉低层的同名 server。
    const auto add = [&](const fs::path& file) {
        const json layer = parse_layer(file);
        const auto list = layer.find("mcpServers");
        if (list == layer.end() || list->is_null()) return;
        if (!list->is_object()) fail(ConfigError::Kind::type, file.string() + "：/mcpServers 应为对象");
        for (const auto& [name, entry] : list->items()) {
            if (entry.is_null()) servers.erase(name);
            else servers[name] = entry;
        }
    };
    std::error_code ec;
    if (const fs::path dir = user_config_dir(); !dir.empty() && fs::exists(dir / "dagent" / "mcp.json", ec))
        add(dir / "dagent" / "mcp.json");
    if (const fs::path project = root / ".mcp.json"; fs::exists(project, ec)) {
        if (trusted) {
            add(project);
        } else {
            untrusted.push_back(project);
            log_app()->warn("项目 {} 未受信任，忽略 {}", root.string(), project.string());
        }
    }
    if (servers.empty()) return {};
    return parse_mcp_servers(json{{"mcpServers", std::move(servers)}}, secrets);
}

fs::path trust_file() {
    const fs::path dir = user_config_dir();
    return dir.empty() ? fs::path{} : dir / "dagent" / "trusted_projects.json";
}

std::string trust_key(const fs::path& project_root) {
    return absolute_under(fs::current_path(), project_root).string();
}

// 信任列表损坏时按空处理：宁可多问一次，也不能因为文件坏了就当成全都信任。
std::vector<std::string> read_trusted() {
    const fs::path file = trust_file();
    std::error_code ec;
    if (file.empty() || !fs::exists(file, ec)) return {};
    try {
        const json root = parse_layer(file);
        std::vector<std::string> out;
        if (const auto list = root.find("trusted"); list != root.end() && list->is_array())
            for (const auto& item : *list)
                if (item.is_string()) out.push_back(item.get<std::string>());
        return out;
    } catch (const ConfigError& e) {
        log_app()->warn("信任列表读取失败，按空处理：{}", e.what());
        return {};
    }
}

} // namespace

std::filesystem::path project_root(const std::filesystem::path& cwd) {
    exec::Command cmd;
    cmd.argv = {"git", "rev-parse", "--show-toplevel"};
    cmd.cwd = cwd;
    cmd.timeout = std::chrono::milliseconds{2000};
    try {
        const exec::Result result = exec::run(cmd);
        if (result.exit_code.value_or(1) == 0) {
            const std::string text = trim_end(result.out.text);
            if (!text.empty()) return absolute_under(cwd, text);
        }
    } catch (const exec::ExecError&) {
        // git 不可用：按不在仓库里处理
    }
    return cwd;
}

bool is_trusted(const std::filesystem::path& root) {
    const std::string key = trust_key(root);
    const std::vector<std::string> trusted = read_trusted();
    return std::find(trusted.begin(), trusted.end(), key) != trusted.end();
}

void trust_project(const std::filesystem::path& root) {
    const fs::path file = trust_file();
    if (file.empty()) fail(ConfigError::Kind::io, "HOME 与 XDG_CONFIG_HOME 都没有设置，无法保存信任列表");
    std::vector<std::string> trusted = read_trusted();
    const std::string key = trust_key(root);
    if (std::find(trusted.begin(), trusted.end(), key) != trusted.end()) return;
    trusted.push_back(key);
    const json out = {{"trusted", trusted}};
    try {
        workspace::write_text(file, out.dump(2) + "\n", workspace::Eol::lf, false);
    } catch (const workspace::WorkspaceError& e) {
        fail(ConfigError::Kind::io, std::format("保存信任列表失败：{}", e.what()));
    }
}

base::Secrets load_secrets(const std::filesystem::path& root) {
    std::vector<fs::path> files;
    // 用户级 .env 放最前面（先读到的优先），并且只接受 0600 或更严的权限：别人能读的密钥文件不用。
    if (const fs::path dir = user_config_dir(); !dir.empty()) {
        const fs::path user_env = dir / "dagent" / ".env";
        std::error_code ec;
        const auto status = fs::status(user_env, ec);
        if (!ec && fs::is_regular_file(status)) {
            const auto loose = fs::perms::group_all | fs::perms::others_all;
            if ((status.permissions() & loose) != fs::perms::none)
                log_app()->warn("{} 的权限过宽（应为 0600），已忽略；执行 chmod 600 后重试", user_env.string());
            else
                files.push_back(user_env);
        }
    }
    files.push_back(root / ".env.dev");
    files.push_back(root / ".env");
    return base::Secrets::load(files);
}

std::vector<mcp::ServerConfig> parse_mcp_servers(const json& root, const base::Secrets& secrets) {
    std::vector<mcp::ServerConfig> servers;
    if (!root.is_object()) fail(ConfigError::Kind::type, "/ 应为对象");
    const auto list = root.find("mcpServers");
    if (list == root.end() || list->is_null()) return servers;
    if (!list->is_object()) fail(ConfigError::Kind::type, "/mcpServers 应为对象");

    // 工具名是 mcp__<server>__<tool>：清理后同名，或名字里带 "__"，不同 server 的工具就会撞名。
    std::map<std::string, std::string> cleaned_names; // 清理后的名字 → 原名
    for (const auto& [name, entry] : list->items()) {
        const std::string where = "/mcpServers/" + name;
        if (!entry.is_object()) fail(ConfigError::Kind::type, where + " 应为对象");

        std::string type = "stdio";
        if (const auto it = entry.find("type"); it != entry.end()) {
            if (!it->is_string()) fail(ConfigError::Kind::type, where + "/type 应为字符串");
            type = it->get<std::string>();
        }
        if (type != "stdio" && type != "http") {
            log_app()->warn("{} 的 type 是 {}，暂不支持，已跳过", where, type);
            continue;
        }

        const std::string cleaned = mcp::sanitize_name(name);
        if (cleaned.empty()) fail(ConfigError::Kind::invalid, "/mcpServers 里有名字为空的 server");
        if (cleaned.find("__") != std::string::npos)
            fail(ConfigError::Kind::invalid,
                 std::format("{}：server 名清理后是 {}，含 \"__\"，会和其他 server 的工具名混淆", where, cleaned));
        if (const auto [it, inserted] = cleaned_names.emplace(cleaned, name); !inserted)
            fail(ConfigError::Kind::invalid,
                 std::format("MCP server {} 和 {} 清理后都是 {}，工具名会冲突，请改名", it->second, name, cleaned));

        const auto expand = [&](const std::string& text, const std::string& field) {
            return expand_placeholders(text, secrets, field);
        };

        mcp::ServerConfig config;
        config.name = name;
        if (type == "stdio") {
            const auto command = entry.find("command");
            if (command == entry.end() || !command->is_string())
                fail(ConfigError::Kind::type, where + "/command 应为字符串");
            config.command.push_back(expand(command->get<std::string>(), where + "/command"));
            if (const auto args = entry.find("args"); args != entry.end() && !args->is_null()) {
                if (!args->is_array()) fail(ConfigError::Kind::type, where + "/args 应为字符串数组");
                for (std::size_t i = 0; i < args->size(); ++i) {
                    const auto& arg = (*args)[i];
                    if (!arg.is_string())
                        fail(ConfigError::Kind::type, std::format("{}/args/{} 应为字符串", where, i));
                    config.command.push_back(expand(arg.get<std::string>(), where + "/args"));
                }
            }
            if (const auto env = entry.find("env"); env != entry.end() && !env->is_null()) {
                if (!env->is_object()) fail(ConfigError::Kind::type, where + "/env 应为对象");
                for (const auto& [key, value] : env->items()) {
                    if (!value.is_string())
                        fail(ConfigError::Kind::type,
                             std::format("{}/env/{} 应为字符串", where, key));
                    config.env.emplace_back(key, expand(value.get<std::string>(), where + "/env"));
                }
            }
        } else {
            config.transport = mcp::Transport::http;
            const auto url = entry.find("url");
            if (url == entry.end() || !url->is_string())
                fail(ConfigError::Kind::type, where + "/url 应为字符串");
            config.url = expand(url->get<std::string>(), where + "/url");
            if (const auto headers = entry.find("headers"); headers != entry.end() && !headers->is_null()) {
                if (!headers->is_object()) fail(ConfigError::Kind::type, where + "/headers 应为对象");
                for (const auto& [key, value] : headers->items()) {
                    if (!value.is_string())
                        fail(ConfigError::Kind::type,
                             std::format("{}/headers/{} 应为字符串", where, key));
                    config.headers.emplace_back(key, expand(value.get<std::string>(), where + "/headers"));
                }
            }
        }
        servers.push_back(std::move(config));
    }
    return servers;
}

Config load_config(const LoadOptions& opt, const base::Secrets& secrets) {
    const fs::path process_cwd = fs::current_path();
    const fs::path cwd = opt.cwd.empty() ? process_cwd : absolute_under(process_cwd, opt.cwd);
    const fs::path root = opt.project_root ? absolute_under(cwd, *opt.project_root) : project_root(cwd);
    const bool trusted = is_trusted(root);
    std::vector<fs::path> untrusted;

    std::vector<fs::path> sources;
    json merged = json::object();
    const auto add = [&](const fs::path& file) {
        std::error_code ec;
        if (!fs::exists(file, ec)) return;
        json layer = parse_layer(file);
        resolve_path_fields(layer, file.parent_path());
        merged.merge_patch(layer);
        sources.push_back(file);
    };

    if (opt.explicit_file) {
        const fs::path file = absolute_under(cwd, *opt.explicit_file);
        if (!fs::exists(file)) fail(ConfigError::Kind::io, "指定的配置文件不存在：" + file.string());
        add(file);
    } else {
        if (const fs::path dir = user_config_dir(); !dir.empty()) add(dir / "dagent" / "config.json");
        const fs::path project_file = root / ".dagent" / "config.json";
        std::error_code ec;
        if (trusted) {
            add(project_file);
        } else if (fs::exists(project_file, ec)) {
            untrusted.push_back(project_file);
            log_app()->warn("项目 {} 未受信任，忽略 {}", root.string(), project_file.string());
        }
    }

    merged.merge_patch(build_override_layer(opt.overrides, cwd));
    warn_unknown(merged);

    const Node node(merged, "");
    Config config;
    config.gateway = map_gateway(node.child("gateway"), secrets);
    config.http = map_http(node.child("http"));
    config.process = map_process(node.child("process"));
    config.files = map_files(node.child("files"));
    config.search = map_search(node.child("search"));
    config.session = map_session(node.child("session"));
    config.log = map_log(node.child("log"));
    config.mcp = map_mcp(node.child("mcp"));
    config.tools = map_tools(node.child("tools"));
    config.context = map_context(node.child("context"));
    config.run = map_run(node.child("run"));
    config.progress = map_progress(node.child("progress"));
    if (const Node v = node.child("permissions"); v.has()) config.permissions = v.str(config.permissions);
    config.credentials = map_credentials(node.child("network"), secrets);
    config.mcp_servers = load_mcp_servers(root, trusted, secrets, untrusted);
    config.sources = std::move(sources);
    config.project_root = root;
    config.project_trusted = trusted;
    config.untrusted_files = std::move(untrusted);
    return config;
}

} // namespace dagent::app
