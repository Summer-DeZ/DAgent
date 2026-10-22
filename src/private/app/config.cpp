#include "app/config_detail.hpp"
#include "app/home.hpp"
#include "app/toolchain.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

#include "base/platform.hpp"
#include "base/path.hpp"
#include "exec/process.hpp"

namespace dagent::app {
namespace fs = std::filesystem;
namespace {

using detail::Node;
using detail::absolute_under;
using detail::expand_env;
using detail::fail;
using detail::log_app;
using detail::parse_file;
using detail::require_private_file;
using detail::size_option;
using json = nlohmann::json;

int count_option(const Node& node, std::string_view key, int minimum = 0) {
    const Node value = node.required(key);
    const int count = value.integer();
    if (count < minimum)
        fail(ConfigError::Kind::invalid, value.pointer() + " must be at least " + std::to_string(minimum));
    return count;
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
    resolve({"decision_review", "threshold_file"});
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
        "memory.enabled", "memory.max_index_bytes", "memory.max_entry_bytes",
        "prompts.system", "prompts.compact", "http.timeout_seconds", "http.connect_timeout_seconds",
        "http.idle_timeout_seconds", "http.max_body_bytes", "http.max_error_body_bytes",
        "http.verify_peer", "http.verify_host", "context.window_tokens",
        "context.safety_margin_tokens", "context.compaction_trigger_percent",
        "context.compaction_target_percent", "files.max_read_bytes", "files.max_write_bytes",
        "search.rg_path", "process.default_timeout_ms", "process.max_output_bytes",
        "process.kill_grace_ms", "process.drain_after_exit_ms", "process.env_deny",
        "run.max_model_calls", "run.max_tool_calls", "run.max_model_retries",
        "run.max_parallel_tasks", "run.max_parallel_tools", "run.max_total_tokens",
        "decision_review.timeout_ms", "decision_review.threshold_profile", "decision_review.threshold_file",
        "subagents.approval.mode", "subagents.approval.max_reviews_per_turn",
        "subagents.approval.review_timeout_ms", "subagents.approval.failure",
        "session.redact_fields", "session.history_scan_limit", "log.max_file_bytes",
        "log.max_files", "log.level", "log.also_stderr", "progress.interval_ms",
        "permissions", "ui.theme_file", "ui.completion_max_files", "ui.appearance.theme", "ui.appearance.mode",
        "ui.appearance.background", "ui.appearance.accent", "ui.appearance.panels",
        "ui.appearance.density", "ui.appearance.icons", "ui.appearance.sidebar",
        "ui.layout.width", "ui.layout.status_panel", "ui.layout.navigator",
        "ui.layout.navigator_sections", "ui.layout.status_sections", "ui.appearance.diff", "ui.appearance.links", "sandbox.version", "sandbox.backend",
        "sandbox.host_access", "sandbox.startup_timeout_ms", "sandbox.network_approval_timeout_ms",
        "sandbox.max_network_requests_per_execution", "sandbox.allowed_targets", "sandbox.denied_targets",
        "mcp.connect_timeout_ms", "mcp.probe_timeout_ms",
        "tools.max_result_bytes", "tools.bash_collect_bytes", "tools.read_default_lines", "tools.read_max_line_bytes",
        "tools.grep_max_matches", "tools.glob_max_files", "tools.bash_max_timeout_ms",
        "tools.mcp_call_timeout_ms", "web.timeout_ms", "web.startup_timeout_ms",
        "web.max_body_bytes", "web.cache_bytes", "web.engines"};
    return keys;
}

bool known_key(std::string_view key) {
    if (known_keys().contains(std::string(key))) return true;
    constexpr std::string_view prefixes[] = {"session.redact_fields", "process.env_deny",
                                              "sandbox.extra_readable", "sandbox.extra_writable",
                                              "sandbox.allowed_targets", "sandbox.denied_targets",
                                              "mcp.mcpServers", "web.engines"};
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

net::HttpOptions map_http(const Node& n) {
    net::HttpOptions o;
    o.timeout = std::chrono::seconds(count_option(n, "timeout_seconds"));
    o.connect_timeout = std::chrono::seconds(count_option(n, "connect_timeout_seconds"));
    o.idle_timeout = std::chrono::seconds(count_option(n, "idle_timeout_seconds"));
    o.max_body_bytes = size_option(n, "max_body_bytes");
    o.max_error_body_bytes = size_option(n, "max_error_body_bytes");
    o.verify_peer = n.required("verify_peer").flag();
    o.verify_host = n.required("verify_host").flag();
    return o;
}

exec::Options map_process(const Node& n) {
    exec::Options o;
    o.default_timeout = std::chrono::milliseconds(count_option(n, "default_timeout_ms"));
    o.max_output_bytes = size_option(n, "max_output_bytes");
    o.kill_grace = std::chrono::milliseconds(count_option(n, "kill_grace_ms"));
    o.drain_after_exit = std::chrono::milliseconds(count_option(n, "drain_after_exit_ms"));
    o.env_deny = n.required("env_deny").strings();
    return o;
}

exec::SandboxOptions map_sandbox(const Node& n, const fs::path& workspace) {
    exec::SandboxOptions options;
    if (auto v = n.child("version"); v.has()) options.version = v.integer(options.version);
    if (options.version != 3) {
        fail(ConfigError::Kind::invalid,
             n.child("version").pointer() + " must be 3: sandbox.version 2 was the removed SRT backend and "
             "1 the removed Landlock backend; set version=3 and backend=mxc");
    }
    options.backend = n.child("backend").str(options.backend);
    if (options.backend != "mxc")
        fail(ConfigError::Kind::invalid, n.child("backend").pointer() + " must be mxc: the SRT backend was removed; "
             "set version=3 and backend=mxc");
    options.host_access = n.child("host_access").str(options.host_access);
    if (options.host_access != "ask_once")
        fail(ConfigError::Kind::invalid, n.child("host_access").pointer() + " must be ask_once");
    options.startup_timeout = std::chrono::milliseconds(
        n.child("startup_timeout_ms").integer(static_cast<int>(options.startup_timeout.count())));
    options.network_approval_timeout = std::chrono::milliseconds(
        n.child("network_approval_timeout_ms").integer(static_cast<int>(options.network_approval_timeout.count())));
    options.max_network_requests_per_execution =
        static_cast<int>(n.child("max_network_requests_per_execution")
                             .integer(options.max_network_requests_per_execution));
    if (options.startup_timeout.count() <= 0 || options.network_approval_timeout.count() <= 0 ||
        options.max_network_requests_per_execution <= 0) {
        fail(ConfigError::Kind::invalid, "sandbox timeouts and the network request budget must be positive");
    }
    const auto paths = [&](std::string_view key) {
        std::vector<fs::path> result;
        for (const std::string& value : n.child(key).strings())
            result.push_back(absolute_under(workspace, value));
        return result;
    };
    options.extra_readable = paths("extra_readable");
    options.extra_writable = paths("extra_writable");
    options.network_allowed = n.child("allowed_targets").strings();
    options.network_denied = n.child("denied_targets").strings();
    return options;
}

workspace::FileOptions map_files(const Node& n) {
    workspace::FileOptions o;
    o.max_read_bytes = size_option(n, "max_read_bytes");
    o.max_write_bytes = size_option(n, "max_write_bytes");
    return o;
}

workspace::SearchOptions map_search(const Node& n) {
    workspace::SearchOptions o;
    o.rg_path = n.required("rg_path").str();
    return o;
}

storage::Options map_session(const Node& n) {
    storage::Options o;
    o.redact_fields = n.required("redact_fields").strings();
    o.history_scan_limit = size_option(n, "history_scan_limit");
    return o;
}

base::LogOptions map_log(const Node& n) {
    base::LogOptions o;
    o.max_file_bytes = size_option(n, "max_file_bytes");
    o.max_files = size_option(n, "max_files");
    o.level = n.required("level").str();
    o.also_stderr = n.required("also_stderr").flag();
    return o;
}

agent::ContextOptions map_context(const Node& n) {
    agent::ContextOptions o;
    o.window_tokens = size_option(n, "window_tokens");
    o.safety_margin_tokens = n.required("safety_margin_tokens").usize();
    o.compaction_trigger_percent = count_option(n, "compaction_trigger_percent", 1);
    o.compaction_target_percent = count_option(n, "compaction_target_percent", 1);
    if (o.safety_margin_tokens >= o.window_tokens || o.compaction_trigger_percent > 100 ||
        o.compaction_target_percent > o.compaction_trigger_percent)
        fail(ConfigError::Kind::invalid, n.pointer() + ": invalid context budget or compaction percentages");
    return o;
}

agent::Limits map_run(const Node& n) {
    agent::Limits o;
    o.max_model_calls = count_option(n, "max_model_calls");
    o.max_total_tokens = n.child("max_total_tokens").usize(0);
    o.max_tool_calls = count_option(n, "max_tool_calls");
    o.max_model_retries = count_option(n, "max_model_retries");
    o.max_parallel_tasks = count_option(n, "max_parallel_tasks", 1);
    o.max_parallel_tools = count_option(n, "max_parallel_tools", 1);
    return o;
}

mcp::Options map_mcp(const Node& n) {
    mcp::Options o;
    o.connect_timeout = std::chrono::milliseconds(count_option(n, "connect_timeout_ms", 1));
    o.probe_timeout = std::chrono::milliseconds(count_option(n, "probe_timeout_ms", 1));
    return o;
}

tools::Options map_tools(const Node& n) {
    tools::Options o;
    o.max_result_bytes = size_option(n, "max_result_bytes");
    o.bash_collect_bytes = size_option(n, "bash_collect_bytes");
    o.read_default_lines = count_option(n, "read_default_lines", 1);
    o.read_max_line_bytes = size_option(n, "read_max_line_bytes");
    o.grep_max_matches = size_option(n, "grep_max_matches");
    o.glob_max_files = size_option(n, "glob_max_files");
    o.bash_max_timeout = std::chrono::milliseconds(count_option(n, "bash_max_timeout_ms", 1));
    o.mcp_call_timeout = std::chrono::milliseconds(count_option(n, "mcp_call_timeout_ms", 1));
    return o;
}

web::Options map_web(const Node& n) {
    web::Options out;
    out.timeout = std::chrono::milliseconds(n.child("timeout_ms").integer(120000));
    out.startup_timeout = std::chrono::milliseconds(n.child("startup_timeout_ms").integer(30000));
    out.max_body_bytes = n.child("max_body_bytes").usize(2 << 20);
    out.cache_bytes = n.child("cache_bytes").usize(16 << 20);
    if (n.child("engines").has()) out.engines = n.child("engines").strings();
    if (out.timeout.count() <= 0 || out.startup_timeout.count() <= 0 || !out.max_body_bytes ||
        out.max_body_bytes > (64 << 20) || out.cache_bytes < out.max_body_bytes || out.engines.empty())
        fail(ConfigError::Kind::invalid, "web requires positive timeouts, max_body_bytes <= 64 MiB, cache_bytes >= max_body_bytes and nonempty engines");
    const auto curl = exec::which("curl");
    if (!curl) fail(ConfigError::Kind::invalid, "web tools require the host curl executable");
    out.curl = *curl;
    return out;
}

} // namespace

std::filesystem::path project_root(const std::filesystem::path& cwd, const exec::Options&) {
    // Repository discovery must not execute configuration-controlled git helpers.
    for (auto directory = cwd;; directory = directory.parent_path()) {
        std::error_code error;
        if (fs::exists(directory / ".git", error)) return directory;
        if (directory == directory.root_path()) return cwd;
    }
}

std::vector<mcp::ServerConfig> parse_mcp_servers(const json& root) {
    std::vector<mcp::ServerConfig> servers;
    const auto list = root.find("mcpServers");
    if (list == root.end() || list->is_null()) return servers;
    if (!list->is_object()) fail(ConfigError::Kind::type, "/mcpServers must be an object");
    std::map<std::string, std::string> cleaned;
    for (const auto& [name, entry] : list->items()) {
        const std::string where = "/mcpServers/" + name;
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
        server.environment = entry.value("environment", "managed");
        if (const auto permissions = entry.find("permissions"); permissions != entry.end()) {
            if (!permissions->is_object()) fail(ConfigError::Kind::type, where + "/permissions must be an object");
            server.profile_present = true;
            const auto strings = [&](const char* key, std::vector<std::string>& out) {
                const auto item = permissions->find(key);
                if (item == permissions->end()) return;
                const std::string field = where + "/permissions/" + key;
                if (!item->is_array()) fail(ConfigError::Kind::type, field + " must be an array");
                for (const auto& value : *item) {
                    if (!value.is_string()) fail(ConfigError::Kind::type, field + " entries must be strings");
                    out.push_back(expand_env(value.get<std::string>(), field));
                }
            };
            std::vector<std::string> read, write;
            strings("read", read);
            strings("write", write);
            strings("network", server.profile_network);
            for (std::string& path : read) server.profile_read.emplace_back(std::move(path));
            for (std::string& path : write) server.profile_write.emplace_back(std::move(path));
        } else {
            log_app()->warn("{} has no permissions profile; the server will not start", where);
        }
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
    const fs::path root = base::canonical_or_absolute_or_lexical(options.root);
    const fs::path cwd = options.cwd.empty() ? fs::current_path() : base::canonical_or_absolute_or_lexical(options.cwd);
    const HomePaths paths(root);
    const fs::path config_file = paths.config;
    json config_json = parse_file(config_file);
    require_private_file(paths.mcp);
    json mcp_json = parse_file(paths.mcp);
    if (config_json.contains("mcp")) fail(ConfigError::Kind::invalid, "MCP configuration belongs in config/mcp.json");
    config_json["mcp"] = std::move(mcp_json);
    resolve_paths(config_json, root);

    ModelCatalog catalog = load_models(root, options.overrides);
    for (const auto& entry : options.overrides) {
        if (!entry.starts_with("@model=")) config_json.merge_patch(override_layer(entry, cwd));
    }
    warn_unknown(config_json);

    const Node node(config_json, "");
    Config config;
    config.root = root;
    config.project_root = cwd;
    config.model_selection_log = std::move(catalog.selection_log);
    config.models = std::move(catalog.models);
    config.model = std::move(catalog.selected);
    config.decision_models = std::move(catalog.decision_models);
    config.decision = std::move(catalog.selected_decision);
    config.ui.theme_file = node.child("ui").child("theme_file").str();
    const auto appearance = node.child("ui").child("appearance");
    config.ui.appearance = resolve_appearance(appearance.exists() ? appearance.raw() : json::object(),
                                              config.root, config.ui.theme_file);
    const auto layout = node.child("ui").child("layout");
    config.ui.layout = resolve_layout(layout.exists() ? layout.raw() : json::object(), config.ui.appearance);

    config.ui.completion_max_files = size_option(node.required("ui"), "completion_max_files");
    config.system_prompt_file = node.child("prompts").child("system").str((paths.prompts / "system.md").string());
    config.compact_prompt_file = node.child("prompts").child("compact").str((paths.prompts / "compact.md").string());
    config.http = map_http(node.required("http"));
    config.process = map_process(node.required("process"));
    config.sandbox = map_sandbox(node.child("sandbox"), cwd);
    config.files = map_files(node.required("files"));
    config.search = map_search(node.required("search"));
    config.session = map_session(node.required("session"));
    config.session.database = paths.database;
    config.session.lock_directory = paths.run / "session-locks";
    config.log = map_log(node.required("log"));
    config.log.file = paths.logs / std::format("dagent-{}.log", base::process_id());
    config.mcp = map_mcp(node.required("mcp"));
    config.mcp.http = config.http;
    config.mcp.process = config.process;
    const auto memory = node.child("memory");
    config.memory.enabled = memory.child("enabled").flag(true);
    if (memory.child("max_index_bytes").has()) config.memory.max_index_bytes = size_option(memory, "max_index_bytes");
    if (memory.child("max_entry_bytes").has()) config.memory.max_entry_bytes = size_option(memory, "max_entry_bytes");
    config.tools = map_tools(node.required("tools"));
    config.tools.web = map_web(node.child("web"));
    config.agent.context = map_context(node.required("context"));
    config.agent.run = map_run(node.required("run"));
    if (const Node decision_review = node.child("decision_review"); decision_review.has()) {
        if (decision_review.child("timeout_ms").has())
            config.decision_review.timeout = std::chrono::milliseconds(
                count_option(decision_review, "timeout_ms", 1));
        config.decision_review.threshold_profile =
            decision_review.child("threshold_profile").str(config.decision_review.threshold_profile);
        config.decision_threshold_file = decision_review.child("threshold_file").str();
        if (config.decision_review.threshold_profile.empty())
            fail(ConfigError::Kind::invalid, "/decision_review/threshold_profile must not be empty");
    }
    const auto approval = node.child("subagents").child("approval");
    const auto approval_mode = approval.child("mode").str("parent_when_unrestricted");
    if (approval_mode == "parent_when_workspace_or_unrestricted")
        config.agent.approval.mode = agent::ParentReviewMode::workspace_and_unrestricted;
    else if (approval_mode == "parent_when_unrestricted")
        config.agent.approval.mode = agent::ParentReviewMode::unrestricted_only;
    else if (approval_mode == "user")
        config.agent.approval.mode = agent::ParentReviewMode::disabled;
    else
        fail(ConfigError::Kind::invalid,
             "/subagents/approval/mode: expected parent_when_workspace_or_unrestricted, "
             "parent_when_unrestricted or user");
    if (approval.child("max_reviews_per_turn").has())
        config.agent.approval.max_reviews_per_turn = count_option(approval, "max_reviews_per_turn");
    if (approval.child("review_timeout_ms").has())
        config.agent.approval.review_timeout = std::chrono::milliseconds(count_option(approval, "review_timeout_ms", 1));
    if (approval.child("failure").str("deny_with_feedback") != "deny_with_feedback")
        fail(ConfigError::Kind::invalid, "/subagents/approval/failure: only deny_with_feedback is supported");
    config.agent.progress.interval = std::chrono::milliseconds(
        count_option(node.required("progress"), "interval_ms"));
    if (auto v = node.child("permissions"); v.has()) {
        const std::string mode = v.str();
        if (mode == "ask") config.permissions = agent::PermissionMode::ask;
        else if (mode == "workspace") config.permissions = agent::PermissionMode::workspace;
        else if (mode == "unrestricted") config.permissions = agent::PermissionMode::unrestricted;
        else fail(ConfigError::Kind::type, v.pointer() + " must be ask, workspace or unrestricted");
    }
    config.mcp_servers = parse_mcp_servers(node.required("mcp").raw());
    Toolchain toolchain(paths);
    config.tools.environments = toolchain.environments();
    config.searxng_environment = toolchain.environment_root("internal/searxng");
    // internal/* 是后端自用的环境（如 internal/searxng），不作为模型可选的 Bash/MCP 环境。
    std::erase_if(config.tools.environments, [](const auto& item) { return item.first.starts_with("internal/"); });
    config.process.environment = config.tools.environments.at("managed").variables;
    config.mcp.process = config.process;
    if (config.search.rg_path.empty()) config.search.rg_path = toolchain.program("rg");
    config.project_root = project_root(cwd, config.process);
    for (auto& server : config.mcp_servers) {
        // profile 里的相对路径按 Home 根解析，与其它受信任配置一致。
        for (auto* profile_paths : {&server.profile_read, &server.profile_write}) {
            for (auto& path : *profile_paths) {
                if (path.is_relative()) path = absolute_under(root, path.string());
            }
        }
    }
    for (auto& server : config.mcp_servers) {
        if (server.transport != mcp::Transport::stdio) continue;
        server.cwd = server.environment == "project" ? cwd : root;
        if (server.environment == "project") continue;
        if (fs::path(server.command.front()).has_parent_path() && fs::path(server.command.front()).is_relative())
            server.command.front() = (root / server.command.front()).lexically_normal().string();
        const auto found = config.tools.environments.find(server.environment);
        if (found == config.tools.environments.end()) fail(ConfigError::Kind::invalid, "unknown MCP environment: " + server.environment);
        auto variables = found->second.variables;
        variables.insert(variables.end(), server.env.begin(), server.env.end());
        server.env = std::move(variables);
    }
    config.subagents = load_subagents(paths.agents, config.models);
    return config;
}

} // namespace dagent::app
