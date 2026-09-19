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
    } catch (const json::exception&) {
        return {}; // 会话文件损坏的条目按 monostate 显示
    }
    return {};
}

} // namespace dagent::tools
