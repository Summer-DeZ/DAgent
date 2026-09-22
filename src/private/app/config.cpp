#include "app/config.hpp"
#include "llm/llm.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

fs::path absolute_path(const fs::path& path) {
    std::error_code ec;
    fs::path result = fs::weakly_canonical(path, ec);
    if (!ec) return result;
    ec.clear();
    result = fs::absolute(path, ec);
    return ec ? path.lexically_normal() : result.lexically_normal();
}

fs::path absolute_under(const fs::path& base, const fs::path& path) {
    return path.is_absolute() ? absolute_path(path) : absolute_path(base / path);
}

std::string trim_end(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

class Node {
public:
    Node() = default;
    Node(const json& value, std::string pointer) : value_(&value), pointer_(std::move(pointer)) {}

    bool exists() const { return value_ != nullptr; }
    bool has() const { return value_ != nullptr && !value_->is_null(); }
    Node child(std::string_view key) const {
        if (!exists()) return {};
        if (!value_->is_object()) wrong("an object");
        const auto it = value_->find(key);
        return it == value_->end() ? Node{} : Node(*it, pointer_ + "/" + std::string(key));
    }
    const json& raw() const { return *value_; }
    const std::string& pointer() const { return pointer_; }
    std::string str(std::string fallback = {}) const {
        if (!has()) return fallback;
        if (!value_->is_string()) wrong("a string");
        return value_->get<std::string>();
    }
    bool flag(bool fallback = false) const {
        if (!has()) return fallback;
        if (!value_->is_boolean()) wrong("a boolean");
        return value_->get<bool>();
    }
    int integer(int fallback = 0) const {
        if (!has()) return fallback;
        if (!value_->is_number_integer() && !value_->is_number_unsigned()) wrong("an integer");
        try { return value_->get<int>(); }
        catch (const json::exception&) { wrong("a 32-bit integer"); }
    }
    std::size_t usize(std::size_t fallback = 0) const {
        if (!has()) return fallback;
        if (value_->is_number_unsigned()) return value_->get<std::size_t>();
        if (value_->is_number_integer()) {
            const auto n = value_->get<std::int64_t>();
            if (n >= 0) return static_cast<std::size_t>(n);
        }
        wrong("a non-negative integer");
    }
    double real(double fallback = 0) const {
        if (!has()) return fallback;
        if (!value_->is_number()) wrong("a number");
        return value_->get<double>();
    }
    std::vector<std::string> strings() const {
        if (!has()) return {};
        if (!value_->is_array()) wrong("an array of strings");
        std::vector<std::string> out;
        for (const auto& item : *value_) {
            if (!item.is_string()) wrong("an array of strings");
            out.push_back(item.get<std::string>());
        }
        return out;
    }

private:
    [[noreturn]] void wrong(std::string_view expected) const {
        fail(ConfigError::Kind::type, std::format("{} must be {}", pointer_, expected));
    }
    const json* value_ = nullptr;
    std::string pointer_;
};

json parse_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) fail(ConfigError::Kind::io, "cannot open config file: " + file.string());
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    try {
        json value = json::parse(text, nullptr, true, true);
        if (!value.is_object()) fail(ConfigError::Kind::parse, file.string() + ": top level must be a JSON object");
        return value;
    } catch (const json::parse_error& e) {
        fail(ConfigError::Kind::parse, file.string() + ": " + e.what());
    }
}

void require_private_file(const fs::path& file) {
    struct stat status {};
    if (::stat(file.c_str(), &status) != 0)
        fail(ConfigError::Kind::io, std::format("cannot stat {}: {}", file.string(), std::strerror(errno)));
    if (!S_ISREG(status.st_mode)) fail(ConfigError::Kind::io, file.string() + " is not a regular file");
    if ((status.st_mode & 0777) != 0600) {
        fail(ConfigError::Kind::invalid,
             std::format("{} must have permissions 0600 because it may contain API keys; run: chmod 600 {}",
                         file.string(), file.string()));
    }
}

void resolve_paths(json& config, const fs::path& root) {
    const auto resolve = [&](std::initializer_list<std::string_view> keys, bool command_like = false) {
        json* node = &config;
        for (const auto key : keys) {
            if (!node->is_object()) return;
            const auto it = node->find(key);
            if (it == node->end()) return;
            node = &*it;
        }
        if (!node->is_string()) return;
        const std::string text = node->get<std::string>();
        if (text.empty() || fs::path(text).is_absolute()) return;
        if (command_like && text.find('/') == std::string::npos) return;
        *node = absolute_under(root, text).string();
    };
    resolve({"prompts", "system"});
    resolve({"prompts", "compact"});
    resolve({"ui", "theme_file"});
    resolve({"search", "rg_path"}, true);
}

json override_layer(const std::string& entry, const fs::path& cwd) {
    const auto eq = entry.find('=');
    if (eq == std::string::npos || eq == 0)
        fail(ConfigError::Kind::type, "invalid config override (expected key=value): " + entry);
    const std::string key = entry.substr(0, eq);
    const std::string raw = entry.substr(eq + 1);
    json value = json::parse(raw, nullptr, false);
    if (value.is_discarded()) value = raw;
    std::vector<std::string> parts;
    std::string_view rest = key;
    while (true) {
        const auto dot = rest.rfind('.');
        parts.emplace_back(rest.substr(dot + 1));
        if (dot == std::string_view::npos) break;
        rest = rest.substr(0, dot);
    }
    json layer = std::move(value);
    for (const auto& part : parts) layer = json{{part, layer}};
    resolve_paths(layer, cwd);
    return layer;
}

const std::set<std::string>& known_keys() {
    static const std::set<std::string> keys = {
        "prompts.system", "prompts.compact", "http.timeout_seconds", "http.connect_timeout_seconds",
        "http.idle_timeout_seconds", "http.max_body_bytes", "http.max_error_body_bytes",
        "http.verify_peer", "http.verify_host", "context.window_tokens",
        "context.safety_margin_tokens", "context.compaction_trigger_percent",
        "context.compaction_target_percent", "files.max_read_bytes", "files.max_write_bytes",
        "search.rg_path", "process.default_timeout_ms", "process.max_output_bytes",
        "process.kill_grace_ms", "process.drain_after_exit_ms", "process.env_deny",
        "run.max_model_calls", "run.max_tool_calls", "run.max_model_retries",
        "run.max_parallel_tasks",
        "session.redact_fields", "log.max_file_bytes",
        "log.max_files", "log.level", "log.also_stderr", "progress.interval_ms",
        "permissions", "ui.theme_file", "sandbox.version", "mcp.connect_timeout_ms", "mcp.probe_timeout_ms",
        "tools.max_result_bytes", "tools.read_default_lines", "tools.read_max_line_bytes",
        "tools.grep_max_matches", "tools.glob_max_files", "tools.bash_max_timeout_ms",
        "tools.mcp_call_timeout_ms"};
    return keys;
}

bool known_key(std::string_view key) {
    if (known_keys().contains(std::string(key))) return true;
    constexpr std::string_view prefixes[] = {"session.redact_fields", "process.env_deny",
                                              "sandbox.extra_readable", "sandbox.extra_writable",
                                              "network.credentials", "mcp.servers"};
    return std::ranges::any_of(prefixes, [&](std::string_view prefix) {
        return key == prefix || (key.starts_with(prefix) && key.size() > prefix.size() &&
                                 key[prefix.size()] == '.');
    });
}

void warn_unknown(const json& config) {
    const json flat = config.flatten();
    for (const auto& [pointer, value] : flat.items()) {
        (void)value;
        std::string key = pointer;
        std::ranges::replace(key, '/', '.');
        if (!key.empty() && key.front() == '.') key.erase(0, 1);
        if (!known_key(key)) log_app()->warn("unknown config key {}", key);
    }
}

std::string env_value(std::string_view name, const std::string& where) {
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr || *value == '\0')
        fail(ConfigError::Kind::invalid, std::format("{} references unset environment variable {}", where, name));
    return value;
}

std::string expand_env(const std::string& text, const std::string& where) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '{') {
            const auto close = text.find('}', i + 2);
            if (close != std::string::npos) {
                out += env_value(std::string_view(text).substr(i + 2, close - i - 2), where);
                i = close + 1;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

std::map<std::string, llm::ProviderConfig> map_models(const Node& node) {
    if (!node.has() || !node.raw().is_object() || node.raw().empty())
        fail(ConfigError::Kind::invalid, "models: at least one named model is required");
    std::map<std::string, llm::ProviderConfig> out;
    for (const auto& [name, entry] : node.raw().items()) {
        const std::string key = "/models/" + name;
        if (name.empty() || !entry.is_object()) fail(ConfigError::Kind::type, key + " must be an object");
        const Node value(entry, key);
        llm::ProviderConfig model;
        model.name = name;
        model.kind = value.child("kind").str(model.kind);
        const auto* info = llm::find_provider(model.kind);
        if (!info) fail(ConfigError::Kind::invalid, key + "/kind: unknown provider " + model.kind);
        model.model = value.child("model").str();
        if (model.model.empty()) fail(ConfigError::Kind::invalid, key + "/model must not be empty");
        model.base_url = value.child("base_url").str(std::string(info->default_base_url));
        if (model.base_url.empty()) fail(ConfigError::Kind::invalid, key + "/base_url is required");
        model.max_tokens = value.child("max_tokens").usize();
        model.temperature = value.child("temperature").real(-1.0);
        model.context_window = value.child("context_window").usize();
        model.send_reasoning_content = value.child("send_reasoning_content").flag();
        model.include_usage = value.child("include_usage").flag(true);
        if (const Node extra = value.child("extra_body"); extra.has()) {
            if (!extra.raw().is_object()) fail(ConfigError::Kind::type, key + "/extra_body must be an object");
            model.extra_body = extra.raw();
        }
        const std::string api_key = value.child("api_key").str();
        model.api_key = api_key.starts_with("env:") ? env_value(std::string_view(api_key).substr(4), key + "/api_key")
                                                     : api_key;
        if (info->needs_api_key && model.api_key.empty())
            fail(ConfigError::Kind::invalid, key + "/api_key is required");
        if (info->needs_max_tokens && model.max_tokens == 0)
            fail(ConfigError::Kind::invalid, key + "/max_tokens must be greater than zero");
        out.emplace(name, std::move(model));
    }
    return out;
}

net::HttpOptions map_http(const Node& n) {
    net::HttpOptions o;
    if (auto v = n.child("timeout_seconds"); v.has()) o.timeout = std::chrono::seconds(v.integer());
    if (auto v = n.child("connect_timeout_seconds"); v.has()) o.connect_timeout = std::chrono::seconds(v.integer());
    if (auto v = n.child("idle_timeout_seconds"); v.has()) o.idle_timeout = std::chrono::seconds(v.integer());
    if (auto v = n.child("max_body_bytes"); v.has()) o.max_body_bytes = v.usize(o.max_body_bytes);
    if (auto v = n.child("max_error_body_bytes"); v.has()) o.max_error_body_bytes = v.usize(o.max_error_body_bytes);
    if (auto v = n.child("verify_peer"); v.has()) o.verify_peer = v.flag(o.verify_peer);
    if (auto v = n.child("verify_host"); v.has()) o.verify_host = v.flag(o.verify_host);
    return o;
}

exec::Options map_process(const Node& n) {
    exec::Options o;
    if (auto v = n.child("default_timeout_ms"); v.has()) o.default_timeout = std::chrono::milliseconds(v.integer());
    if (auto v = n.child("max_output_bytes"); v.has()) o.max_output_bytes = v.usize(o.max_output_bytes);
    if (auto v = n.child("kill_grace_ms"); v.has()) o.kill_grace = std::chrono::milliseconds(v.integer());
    if (auto v = n.child("drain_after_exit_ms"); v.has()) o.drain_after_exit = std::chrono::milliseconds(v.integer());
    if (auto v = n.child("env_deny"); v.has()) o.env_deny = v.strings();
    return o;
}

exec::SandboxOptions map_sandbox(const Node& n, const fs::path& workspace) {
    exec::SandboxOptions options;
    if (auto v = n.child("version"); v.has()) options.version = v.integer(options.version);
    if (options.version != 1)
        fail(ConfigError::Kind::invalid, n.child("version").pointer() + " must be 1");
    const auto paths = [&](std::string_view key) {
        std::vector<fs::path> result;
        for (const std::string& value : n.child(key).strings())
            result.push_back(absolute_under(workspace, value));
        return result;
    };
    options.extra_readable = paths("extra_readable");
    options.extra_writable = paths("extra_writable");
    return options;
}

workspace::FileOptions map_files(const Node& n) {
    workspace::FileOptions o;
    if (auto v = n.child("max_read_bytes"); v.has()) o.max_read_bytes = v.usize(o.max_read_bytes);
    if (auto v = n.child("max_write_bytes"); v.has()) o.max_write_bytes = v.usize(o.max_write_bytes);
    return o;
}

workspace::SearchOptions map_search(const Node& n) {
    workspace::SearchOptions o;
    if (auto v = n.child("rg_path"); v.has()) o.rg_path = v.str();
    return o;
}

session::Options map_session(const Node& n) {
    session::Options o;
    if (auto v = n.child("redact_fields"); v.has()) o.redact_fields = v.strings();
    return o;
}

base::LogOptions map_log(const Node& n) {
    base::LogOptions o;
    if (auto v = n.child("max_file_bytes"); v.has()) o.max_file_bytes = v.usize(o.max_file_bytes);
    if (auto v = n.child("max_files"); v.has()) o.max_files = v.usize(o.max_files);
    if (auto v = n.child("level"); v.has()) o.level = v.str(o.level);
    if (auto v = n.child("also_stderr"); v.has()) o.also_stderr = v.flag(o.also_stderr);
    return o;
}

agent::ContextOptions map_context(const Node& n) {
    agent::ContextOptions o;
    if (auto v = n.child("window_tokens"); v.has()) o.window_tokens = v.usize(o.window_tokens);
    if (auto v = n.child("safety_margin_tokens"); v.has()) o.safety_margin_tokens = v.usize(o.safety_margin_tokens);
    if (auto v = n.child("compaction_trigger_percent"); v.has()) o.compaction_trigger_percent = v.integer(o.compaction_trigger_percent);
    if (auto v = n.child("compaction_target_percent"); v.has()) o.compaction_target_percent = v.integer(o.compaction_target_percent);
    return o;
}

agent::Limits map_run(const Node& n) {
    agent::Limits o;
    if (auto v = n.child("max_model_calls"); v.has()) o.max_model_calls = v.integer(o.max_model_calls);
    if (auto v = n.child("max_tool_calls"); v.has()) o.max_tool_calls = v.integer(o.max_tool_calls);
    if (auto v = n.child("max_model_retries"); v.has()) o.max_model_retries = v.integer(o.max_model_retries);
    if (auto v = n.child("max_parallel_tasks"); v.has())
        o.max_parallel_tasks = std::clamp(v.integer(o.max_parallel_tasks), 3, 16);
    return o;
}

mcp::Options map_mcp(const Node& n) {
    mcp::Options o;
    if (auto v = n.child("connect_timeout_ms"); v.has()) o.connect_timeout = std::chrono::milliseconds(v.integer());
    if (auto v = n.child("probe_timeout_ms"); v.has()) o.probe_timeout = std::chrono::milliseconds(v.integer());
    return o;
}

tools::Options map_tools(const Node& n) {
    tools::Options o;
    if (auto v = n.child("max_result_bytes"); v.has()) o.max_result_bytes = v.usize(o.max_result_bytes);
    if (auto v = n.child("read_default_lines"); v.has()) o.read_default_lines = v.integer(o.read_default_lines);
    if (auto v = n.child("read_max_line_bytes"); v.has()) o.read_max_line_bytes = v.usize(o.read_max_line_bytes);
    if (auto v = n.child("grep_max_matches"); v.has()) o.grep_max_matches = v.usize(o.grep_max_matches);
    if (auto v = n.child("glob_max_files"); v.has()) o.glob_max_files = v.usize(o.glob_max_files);
    if (auto v = n.child("bash_max_timeout_ms"); v.has()) o.bash_max_timeout = std::chrono::milliseconds(v.integer());
    if (auto v = n.child("mcp_call_timeout_ms"); v.has()) o.mcp_call_timeout = std::chrono::milliseconds(v.integer());
    return o;
}

std::map<std::string, std::string> map_credentials(const Node& n) {
    std::map<std::string, std::string> out;
    const Node values = n.child("credentials");
    if (!values.has()) return out;
    if (!values.raw().is_object()) fail(ConfigError::Kind::type, values.pointer() + " must be an object");
    for (const auto& [host, value] : values.raw().items()) {
        if (!value.is_string()) fail(ConfigError::Kind::type, values.pointer() + "/" + host + " must be a string");
        const std::string env = value.get<std::string>();
        if (const char* secret = std::getenv(env.c_str()); secret != nullptr) out[host] = secret;
        else log_app()->warn("network.credentials.{} references unset environment variable {}", host, env);
    }
    return out;
}

bool blank(std::string_view text) {
    return text.find_first_not_of(" \t\r") == std::string_view::npos;
}

std::string trim(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) return {};
    const std::size_t end = text.find_last_not_of(" \t\r");
    return std::string(text.substr(begin, end - begin + 1));
}

std::vector<std::string> split_list(std::string_view value) {
    std::vector<std::string> items;
    for (std::string_view rest = value;;) {
        const auto comma = rest.find(',');
        std::string item = trim(rest.substr(0, comma));
        if (!item.empty()) items.push_back(std::move(item));
        if (comma == std::string_view::npos) break;
        rest = rest.substr(comma + 1);
    }
    return items;
}

std::vector<std::string> parse_tools(std::string_view value) {
    std::string_view inner = value;
    if (inner.starts_with('[') && inner.ends_with(']')) inner = inner.substr(1, inner.size() - 2);
    return split_list(inner);
}

int parse_int(const fs::path& file, std::size_t line, std::string_view value, std::string_view key) {
    int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result < 0)
        fail(ConfigError::Kind::invalid, std::format("{}:{}: {} must be a non-negative integer", file.filename().string(), line, key));
    return result;
}

agent::SubagentDef parse_subagent(const fs::path& file, const std::string& text,
                                  const std::map<std::string, llm::ProviderConfig>& models) {
    const auto fail_at = [&](ConfigError::Kind kind, std::size_t line, const std::string& what) {
        fail(kind, std::format("{}:{}: {}", file.filename().string(), line, what));
    };
    std::vector<std::string> lines;
    for (std::string_view rest = text;;) {
        const auto newline = rest.find('\n');
        std::string line(rest.substr(0, newline));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (newline == std::string_view::npos) break;
        rest = rest.substr(newline + 1);
    }
    if (lines.empty() || lines.front() != "---")
        fail_at(ConfigError::Kind::parse, 1, "subagent file must start with a '---' frontmatter block");
    std::size_t end = 0;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i] == "---") { end = i; break; }
    }
    if (end == 0) fail_at(ConfigError::Kind::parse, 1, "subagent frontmatter is missing its closing '---'");

    agent::SubagentDef def;
    std::size_t name_line = 1, permission_line = 1, model_line = 1;
    bool tools_open = false;
    for (std::size_t i = 1; i < end; ++i) {
        const std::string& line = lines[i];
        if (blank(line) || line.front() == '#') continue;
        if (line.front() == '-') {
            if (!tools_open) fail_at(ConfigError::Kind::parse, i + 1, "list item outside a tools key");
            std::string item = trim(line.substr(1));
            if (!item.empty()) def.tools.push_back(std::move(item));
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) fail_at(ConfigError::Kind::parse, i + 1, "expected 'key: value'");
        tools_open = false;
        const std::string key = trim(std::string_view(line).substr(0, colon));
        const std::string value = trim(std::string_view(line).substr(colon + 1));
        if (key == "name") { def.name = value; name_line = i + 1; }
        else if (key == "description") def.description = value;
        else if (key == "model") { def.model = value; model_line = i + 1; }
        else if (key == "tools") { def.tools = parse_tools(value); tools_open = true; }
        else if (key == "permission") { def.permission = value; permission_line = i + 1; }
        else if (key == "max_model_calls") def.max_model_calls = parse_int(file, i + 1, value, key);
        else if (key == "max_tool_calls") def.max_tool_calls = parse_int(file, i + 1, value, key);
        else log_app()->warn("{}:{}: unknown subagent key {}", file.filename().string(), i + 1, key);
    }
    if (def.name.empty()) fail_at(ConfigError::Kind::invalid, name_line, "name must not be empty");
    if (def.name.find_first_of(" \t") != std::string::npos || def.name.find("__") != std::string::npos)
        fail_at(ConfigError::Kind::invalid, name_line, "name must not contain whitespace or '__'");
    if (def.permission != "inherit" && def.permission != "read_only" && def.permission != "ask")
        fail_at(ConfigError::Kind::invalid, permission_line, "permission must be inherit, read_only or ask");
    if (!def.model.empty() && !models.contains(def.model))
        fail_at(ConfigError::Kind::invalid, model_line, std::format("model {} is not defined in models.json", def.model));

    std::string body;
    for (std::size_t i = end + 1; i < lines.size(); ++i) {
        body += lines[i];
        if (i + 1 < lines.size()) body += '\n';
    }
    while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back()))) body.pop_back();
    if (body.empty()) fail_at(ConfigError::Kind::invalid, end + 1, "system prompt body must not be empty");
    def.system_prompt = std::move(body);
    return def;
}

} // namespace

InstallationPaths installation_paths() {
    fs::path root;
    if (const char* value = std::getenv("DAGENT_HOME"); value != nullptr && *value != '\0') {
        root = absolute_path(value);
    } else {
#ifdef DAGENT_DEV_HOME
        root = absolute_path(DAGENT_DEV_HOME);
#else
        std::array<char, 4096> target{};
        const ssize_t size = ::readlink("/proc/self/exe", target.data(), target.size() - 1);
        if (size < 0) fail(ConfigError::Kind::io, std::format("cannot resolve /proc/self/exe: {}", std::strerror(errno)));
        root = fs::path(std::string(target.data(), static_cast<std::size_t>(size))).parent_path();
#endif
    }
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        fail(ConfigError::Kind::io, "installation root does not exist or is not a directory: " + root.string());

    const fs::path probe = root / std::format(".dagent-write-{}", ::getpid());
    const int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        fail(ConfigError::Kind::io,
             std::format("installation root {} is not writable (sessions and logs are stored there): {}. Set DAGENT_HOME to a writable directory",
                         root.string(), std::strerror(errno)));
    }
    ::close(fd);
    ::unlink(probe.c_str());
    return {root, root / "config.json", root / "models.json", root / "dagent.db", root / "logs"};
}

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
    } catch (const exec::ExecError&) {}
    return cwd;
}

std::vector<mcp::ServerConfig> parse_mcp_servers(const json& root) {
    std::vector<mcp::ServerConfig> servers;
    const auto list = root.find("mcpServers");
    if (list == root.end() || list->is_null()) return servers;
    if (!list->is_object()) fail(ConfigError::Kind::type, "/mcpServers must be an object");
    std::map<std::string, std::string> cleaned;
    for (const auto& [name, entry] : list->items()) {
        const std::string where = "/mcp/servers/" + name;
        if (!entry.is_object()) fail(ConfigError::Kind::type, where + " must be an object");
        const std::string normalized = mcp::sanitize_name(name);
        if (normalized.empty() || normalized.find("__") != std::string::npos)
            fail(ConfigError::Kind::invalid, where + " has an invalid server name");
        if (const auto [it, inserted] = cleaned.emplace(normalized, name); !inserted)
            fail(ConfigError::Kind::invalid, std::format("MCP servers {} and {} normalize to the same name", it->second, name));

        const std::string type = entry.value("type", "stdio");
        if (type != "stdio" && type != "http") {
            log_app()->warn("{} has unsupported type {}; ignored", where, type);
            continue;
        }
        mcp::ServerConfig server;
        server.name = name;
        if (type == "stdio") {
            const auto command = entry.find("command");
            if (command == entry.end() || !command->is_string()) fail(ConfigError::Kind::type, where + "/command must be a string");
            server.command.push_back(expand_env(command->get<std::string>(), where + "/command"));
            if (const auto args = entry.find("args"); args != entry.end()) {
                if (!args->is_array()) fail(ConfigError::Kind::type, where + "/args must be an array");
                for (const auto& arg : *args) {
                    if (!arg.is_string()) fail(ConfigError::Kind::type, where + "/args entries must be strings");
                    server.command.push_back(expand_env(arg.get<std::string>(), where + "/args"));
                }
            }
            if (const auto env = entry.find("env"); env != entry.end()) {
                if (!env->is_object()) fail(ConfigError::Kind::type, where + "/env must be an object");
                for (const auto& [key, value] : env->items()) {
                    if (!value.is_string()) fail(ConfigError::Kind::type, where + "/env values must be strings");
                    server.env.emplace_back(key, expand_env(value.get<std::string>(), where + "/env"));
                }
            }
        } else {
            server.transport = mcp::Transport::http;
            const auto url = entry.find("url");
            if (url == entry.end() || !url->is_string()) fail(ConfigError::Kind::type, where + "/url must be a string");
            server.url = expand_env(url->get<std::string>(), where + "/url");
            if (const auto headers = entry.find("headers"); headers != entry.end()) {
                if (!headers->is_object()) fail(ConfigError::Kind::type, where + "/headers must be an object");
                for (const auto& [key, value] : headers->items()) {
                    if (!value.is_string()) fail(ConfigError::Kind::type, where + "/headers values must be strings");
                    server.headers.emplace_back(key, expand_env(value.get<std::string>(), where + "/headers"));
                }
            }
        }
        servers.push_back(std::move(server));
    }
    return servers;
}

Config load_config(const LoadOptions& options) {
    const fs::path root = absolute_path(options.root);
    const fs::path cwd = options.cwd.empty() ? fs::current_path() : absolute_path(options.cwd);
    const fs::path config_file = root / "config.json";
    const fs::path models_file = root / "models.json";
    require_private_file(models_file);
    json config_json = parse_file(config_file);
    const json models_json = parse_file(models_file);
    resolve_paths(config_json, root);

    std::vector<std::string> model_log;
    std::string selected = Node(models_json, "").child("default").str();
    json models = Node(models_json, "").child("models").has()
                      ? Node(models_json, "").child("models").raw() : json{};
    for (const auto& entry : options.overrides) {
        if (entry.starts_with("@model=")) {
            const std::string choice = entry.substr(7);
            if (models.is_object() && models.contains(choice)) {
                selected = choice;
                model_log.push_back(std::format("--model {} selected a named configuration", choice));
            } else {
                if (!models.is_object() || !models.contains(selected))
                    fail(ConfigError::Kind::invalid, "select an existing model before overriding its model id");
                models[selected]["model"] = choice;
                model_log.push_back(std::format("--model {} overrides model id in {}", choice, selected));
            }
        } else {
            config_json.merge_patch(override_layer(entry, cwd));
        }
    }
    warn_unknown(config_json);

    const Node node(config_json, "");
    Config config;
    config.root = root;
    config.project_root = project_root(cwd);
    config.model_selection_log = std::move(model_log);
    config.models = map_models(Node(models, "/models"));
    config.model = std::move(selected);
    if (!config.models.contains(config.model))
        fail(ConfigError::Kind::invalid, "models.json default names an unknown model: " + config.model);
    config.ui.theme_file = node.child("ui").child("theme_file").str();
    config.system_prompt_file = node.child("prompts").child("system").str((root / "system.md").string());
    config.compact_prompt_file = node.child("prompts").child("compact").str((root / "compact.md").string());
    config.http = map_http(node.child("http"));
    config.process = map_process(node.child("process"));
    config.sandbox = map_sandbox(node.child("sandbox"), cwd);
    config.files = map_files(node.child("files"));
    config.search = map_search(node.child("search"));
    config.session = map_session(node.child("session"));
    config.session.database = root / "dagent.db";
    config.log = map_log(node.child("log"));
    config.log.file = root / "logs" / std::format("dagent-{}.log", ::getpid());
    config.mcp = map_mcp(node.child("mcp"));
    config.tools = map_tools(node.child("tools"));
    config.agent.context = map_context(node.child("context"));
    config.agent.run = map_run(node.child("run"));
    if (auto v = node.child("progress").child("interval_ms"); v.has())
        config.agent.progress.interval = std::chrono::milliseconds(v.integer());
    if (auto v = node.child("permissions"); v.has()) {
        const std::string mode = v.str();
        if (mode == "ask") config.agent.permissions = agent::PermissionMode::ask;
        else if (mode == "workspace") config.agent.permissions = agent::PermissionMode::workspace;
        else if (mode == "unrestricted") config.agent.permissions = agent::PermissionMode::unrestricted;
        else fail(ConfigError::Kind::type, v.pointer() + " must be ask, workspace or unrestricted");
    }
    config.credentials = map_credentials(node.child("network"));
    if (const Node servers = node.child("mcp").child("servers"); servers.has())
        config.mcp_servers = parse_mcp_servers(json{{"mcpServers", servers.raw()}});
    config.subagents = load_subagents(root / "agents", config.models);
    return config;
}

std::vector<agent::SubagentDef> load_subagents(
    const fs::path& dir, const std::map<std::string, llm::ProviderConfig>& models) {
    std::vector<agent::SubagentDef> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".md") files.push_back(entry.path());
    }
    if (ec) fail(ConfigError::Kind::io, "cannot list " + dir.string() + ": " + ec.message());
    std::ranges::sort(files); // 工具列表顺序影响前缀缓存，按文件名固定
    std::set<std::string> names;
    for (const fs::path& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) fail(ConfigError::Kind::io, "cannot open subagent file: " + file.string());
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        agent::SubagentDef def = parse_subagent(file, text, models);
        if (!names.insert(def.name).second)
            fail(ConfigError::Kind::invalid, file.filename().string() + ": duplicate subagent name " + def.name);
        out.push_back(std::move(def));
    }
    return out;
}

llm::ProviderConfig add_model(const fs::path& root_path,
                                const llm::ProviderConfig& model) {
    const fs::path root = absolute_path(root_path);
    const fs::path file = root / "models.json";
    require_private_file(file);
    json document = parse_file(file);
    json& models = document["models"];
    if (!models.is_object()) fail(ConfigError::Kind::type, "/models must be an object");
    if (model.name.empty()) fail(ConfigError::Kind::invalid, "model configuration name must not be empty");
    if (models.contains(model.name))
        fail(ConfigError::Kind::invalid, "model configuration already exists: " + model.name);

    json entry{{"kind", model.kind},
               {"base_url", model.base_url},
               {"model", model.model},
               {"max_tokens", model.max_tokens},
               {"context_window", model.context_window},
               {"send_reasoning_content", model.send_reasoning_content},
               {"include_usage", model.include_usage}};
    if (!model.api_key.empty()) entry["api_key"] = model.api_key;
    if (model.temperature >= 0.0) entry["temperature"] = model.temperature;
    if (!model.extra_body.empty()) entry["extra_body"] = model.extra_body;
    models[model.name] = std::move(entry);

    const auto parsed = map_models(Node(models, "/models"));
    const auto selected = parsed.find(model.name);
    if (selected == parsed.end()) fail(ConfigError::Kind::invalid, "failed to add model " + model.name);

    const std::optional<workspace::Stamp> stamp = workspace::stamp_of(file);
    workspace::write_text(file, document.dump(2) + "\n", workspace::Eol::lf, false, stamp);
    require_private_file(file);
    return selected->second;
}

} // namespace dagent::app
