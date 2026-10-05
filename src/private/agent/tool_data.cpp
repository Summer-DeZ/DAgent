#include "agent/tool_data.hpp"

#include <utility>

#include "agent/port_journal.hpp"

namespace dagent::agent {
namespace {

using json = nlohmann::json;

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

constexpr std::string_view kRead = "read";
constexpr std::string_view kChange = "change";
constexpr std::string_view kBash = "bash";
constexpr std::string_view kGrep = "grep";
constexpr std::string_view kGlob = "glob";
constexpr std::string_view kMcp = "mcp";
constexpr std::string_view kTodo = "todo";
constexpr std::string_view kAsk = "ask";
constexpr std::string_view kTask = "task";

} // namespace

// 序列化函数放在 dagent::agent 名字空间供 ADL 查找；所有展示字段按当前格式读取。
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WebView, operation, query, url, title, content_type, output, status,
    offset, next_offset, total_bytes, cached, truncated, network_targets, elapsed_ms)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SkillView, name, path)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReadView, path, start_line, end_line, total_lines,
                                                truncated, directory)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileChangeView, path, diff, added, removed, created)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GrepLine, path, text, line, spans, is_context)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GrepView, pattern, lines, truncated)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GlobView, pattern, files, truncated)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(McpView, server, tool, content, structured,
                                                disconnected)

inline void to_json(nlohmann::json& j, const TodoItem& item) {
    constexpr std::string_view names[] = {"todo", "doing", "done", "dropped"};
    j = {{"text", item.text}, {"state", names[static_cast<std::size_t>(item.state)]}};
}

inline void from_json(const nlohmann::json& j, TodoItem& item) {
    item.text = j.at("text").get<std::string>();
    const std::string state = j.at("state").get<std::string>();
    if (state == "todo") item.state = TodoItem::State::todo;
    else if (state == "doing") item.state = TodoItem::State::doing;
    else if (state == "done") item.state = TodoItem::State::done;
    else if (state == "dropped") item.state = TodoItem::State::dropped;
    else throw RecordError(RecordError::Kind::corrupt, "unknown todo state: " + state);
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TodoView, items)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AskOption, label, description)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AskView, header, prompt, options, selected, other,
                                                multi_select, allow_other, cancelled)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TaskStep, summary, is_error)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TaskView, agent, task, session_id, result, steps,
                                                model_calls, tool_calls, seconds, interrupted)

// BashView 的 exit_code / signal 是 std::optional<int>：本项目用的 nlohmann 开着隐式转换，
// 这份配置不提供 optional 的序列化，BashView 手写；null 表示没有退出码或信号。
inline void to_json(nlohmann::json& j, const BashView& v) {
    j["command"] = v.command;
    j["output"] = v.output;
    j["exit_code"] = v.exit_code ? nlohmann::json(*v.exit_code) : nlohmann::json(nullptr);
    j["signal"] = v.signal ? nlohmann::json(*v.signal) : nlohmann::json(nullptr);
    j["timed_out"] = v.timed_out;
    j["interrupted"] = v.interrupted;
    j["sandbox"] = v.sandbox;
    j["backend"] = v.backend;
    j["grant_source"] = v.grant_source;
    j["analysis_version"] = v.analysis_version;
    j["allow_network"] = v.allow_network;
    j["allow_local_sockets"] = v.allow_local_sockets;
    j["private_tmp"] = v.private_tmp;
    j["protect_sensitive_names"] = v.protect_sensitive_names;
    j["readable"] = v.readable;
    j["writable"] = v.writable;
    j["protected_read"] = v.protected_read;
    j["protected_write"] = v.protected_write;
    j["network_targets"] = v.network_targets;
    j["elapsed_ms"] = v.elapsed_ms;
}

inline void from_json(const nlohmann::json& j, BashView& v) {
    j.at("command").get_to(v.command);
    j.at("output").get_to(v.output);
    j.at("timed_out").get_to(v.timed_out);
    j.at("interrupted").get_to(v.interrupted);
    j.at("sandbox").get_to(v.sandbox);
    j.at("backend").get_to(v.backend);
    j.at("grant_source").get_to(v.grant_source);
    j.at("analysis_version").get_to(v.analysis_version);
    j.at("allow_network").get_to(v.allow_network);
    j.at("allow_local_sockets").get_to(v.allow_local_sockets);
    j.at("private_tmp").get_to(v.private_tmp);
    j.at("protect_sensitive_names").get_to(v.protect_sensitive_names);
    j.at("readable").get_to(v.readable);
    j.at("writable").get_to(v.writable);
    j.at("protected_read").get_to(v.protected_read);
    j.at("protected_write").get_to(v.protected_write);
    j.at("network_targets").get_to(v.network_targets);
    j.at("elapsed_ms").get_to(v.elapsed_ms);
    const auto& exit_code = j.at("exit_code");
    v.exit_code = exit_code.is_null() ? std::nullopt : std::optional<int>(exit_code.get<int>());
    const auto& signal = j.at("signal");
    v.signal = signal.is_null() ? std::nullopt : std::optional<int>(signal.get<int>());
}

json to_json(const View& view) {
    json out;
    std::visit(Overloaded{
                   [&](std::monostate) { out = json{{"kind", nullptr}}; },
                   [&](const ReadView& v) { out = json{{"kind", kRead}}; out.update(json(v)); },
                   [&](const FileChangeView& v) { out = json{{"kind", kChange}}; out.update(json(v)); },
                   [&](const BashView& v) { out = json{{"kind", kBash}}; out.update(json(v)); },
                   [&](const GrepView& v) { out = json{{"kind", kGrep}}; out.update(json(v)); },
                   [&](const GlobView& v) { out = json{{"kind", kGlob}}; out.update(json(v)); },
                   [&](const WebView& v) { out = json{{"kind", "web"}}; out.update(json(v)); },
                   [&](const McpView& v) { out = json{{"kind", kMcp}}; out.update(json(v)); },
                   [&](const TodoView& v) { out = json{{"kind", kTodo}}; out.update(json(v)); },
                   [&](const AskView& v) { out = json{{"kind", kAsk}}; out.update(json(v)); },
                   [&](const TaskView& v) { out = json{{"kind", kTask}}; out.update(json(v)); },
                   [&](const SkillView& v) { out = json{{"kind", "skill"}}; out.update(json(v)); },
               },
               view);
    return out;
}

View view_from_json(const json& data) {
    try {
        const auto& kind = data.at("kind");
        if (kind.is_null()) return {};
        const std::string name = kind.get<std::string>();
        if (name == "skill") return data.get<SkillView>();
        if (name == "web") return data.get<WebView>();
        if (name == kRead) return data.get<ReadView>();
        if (name == kChange) return data.get<FileChangeView>();
        if (name == kBash) return data.get<BashView>();
        if (name == kGrep) return data.get<GrepView>();
        if (name == kGlob) return data.get<GlobView>();
        if (name == kMcp) return data.get<McpView>();
        if (name == kTodo) return data.get<TodoView>();
        if (name == kAsk) return data.get<AskView>();
        if (name == kTask) return data.get<TaskView>();
    } catch (const json::exception& error) {
        throw RecordError(RecordError::Kind::corrupt, std::string("invalid tool view: ") + error.what());
    }
    throw RecordError(RecordError::Kind::corrupt, "unknown tool view kind");
}

} // namespace dagent::agent
