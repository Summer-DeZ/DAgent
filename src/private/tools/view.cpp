#include "tools/view.hpp"

#include <utility>

namespace dagent::tools {
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

// 序列化函数必须生成在 dagent::tools 名字空间里（ADL 才找得到）。缺字段时取默认值：
// 以后给结构体加字段，旧会话照样读得出来。
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ReadView, path, start_line, end_line, total_lines,
                                                truncated, directory)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FileChangeView, path, diff, added, removed, created)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GrepLine, path, text, line, spans, is_context)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GrepView, pattern, lines, truncated)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GlobView, pattern, files, truncated)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(McpView, server, tool, content, structured,
                                                disconnected)

inline void to_json(nlohmann::json& j, const TodoItem& item) {
    constexpr std::string_view names[] = {"todo", "doing", "done", "dropped"};
    j = {{"text", item.text}, {"state", names[static_cast<std::size_t>(item.state)]}};
}

inline void from_json(const nlohmann::json& j, TodoItem& item) {
    item.text = j.value("text", "");
    const std::string state = j.value("state", "todo");
    item.state = state == "doing" ? TodoItem::State::doing
               : state == "done" ? TodoItem::State::done
               : state == "dropped" ? TodoItem::State::dropped
                                     : TodoItem::State::todo;
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TodoView, items)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AskOption, label, description)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AskView, header, prompt, options, selected, other,
                                                multi_select, allow_other, cancelled)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TaskStep, summary, is_error)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TaskView, agent, task, session_id, result, steps,
                                                model_calls, tool_calls, seconds, interrupted)

// BashView 的 exit_code / signal 是 std::optional<int>：本项目用的 nlohmann 开着隐式转换，
// 这份配置不提供 optional 的序列化，BashView 手写（缺字段取默认值，null 表示没有）。
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
    const BashView d;
    v.command = j.value("command", d.command);
    v.output = j.value("output", d.output);
    if (const auto it = j.find("exit_code"); it != j.end() && it->is_number_integer())
        v.exit_code = it->get<int>();
    if (const auto it = j.find("signal"); it != j.end() && it->is_number_integer())
        v.signal = it->get<int>();
    v.timed_out = j.value("timed_out", d.timed_out);
    v.interrupted = j.value("interrupted", d.interrupted);
    v.sandbox = j.value("sandbox", d.sandbox);
    v.backend = j.value("backend", d.backend);
    v.grant_source = j.value("grant_source", d.grant_source);
    v.analysis_version = j.value("analysis_version", d.analysis_version);
    v.allow_network = j.value("allow_network", d.allow_network);
    v.allow_local_sockets = j.value("allow_local_sockets", d.allow_local_sockets);
    v.private_tmp = j.value("private_tmp", d.private_tmp);
    v.protect_sensitive_names = j.value("protect_sensitive_names", d.protect_sensitive_names);
    v.readable = j.value("readable", d.readable);
    v.writable = j.value("writable", d.writable);
    v.protected_read = j.value("protected_read", d.protected_read);
    v.protected_write = j.value("protected_write", d.protected_write);
    v.network_targets = j.value("network_targets", d.network_targets);
    v.elapsed_ms = j.value("elapsed_ms", d.elapsed_ms);
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
                   [&](const McpView& v) { out = json{{"kind", kMcp}}; out.update(json(v)); },
                   [&](const TodoView& v) { out = json{{"kind", kTodo}}; out.update(json(v)); },
                   [&](const AskView& v) { out = json{{"kind", kAsk}}; out.update(json(v)); },
                   [&](const TaskView& v) { out = json{{"kind", kTask}}; out.update(json(v)); },
               },
               view);
    return out;
}

View view_from_json(const json& data) {
    if (!data.is_object()) return {};
    const auto kind = data.find("kind");
    if (kind == data.end() || !kind->is_string()) return {};
    try {
        const std::string name = kind->get<std::string>();
        if (name == kRead) return data.get<ReadView>();
        if (name == kChange) return data.get<FileChangeView>();
        if (name == kBash) return data.get<BashView>();
        if (name == kGrep) return data.get<GrepView>();
        if (name == kGlob) return data.get<GlobView>();
        if (name == kMcp) return data.get<McpView>();
        if (name == kTodo) return data.get<TodoView>();
        if (name == kAsk) return data.get<AskView>();
        if (name == kTask) return data.get<TaskView>();
    } catch (const json::exception&) {
        return {}; // 会话文件损坏的条目按 monostate 显示
    }
    return {};
}

} // namespace dagent::tools
