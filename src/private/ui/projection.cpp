#include "ui/projection.hpp"

#include <utility>

namespace dagent::ui {
namespace {

using nlohmann::json;

/// 协议把“没有值”编码为 null；字符串字段读取统一在这里回退为空。
std::string text_field(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

TurnStatus status_from(std::string_view value) {
    if (value == "interrupted") return TurnStatus::interrupted;
    if (value == "denied") return TurnStatus::denied;
    if (value == "limit") return TurnStatus::limit;
    if (value == "failed") return TurnStatus::failed;
    return TurnStatus::done;
}

TodoList todo_from(const json& items) {
    TodoList out;
    if (!items.is_array()) return out;
    for (const json& item : items) {
        TodoItem entry;
        entry.text = item.value("text", "");
        const std::string state = item.value("state", "todo");
        entry.state = state == "doing" ? TodoItem::State::doing
                    : state == "done" ? TodoItem::State::done
                    : state == "dropped" ? TodoItem::State::dropped
                                         : TodoItem::State::todo;
        out.push_back(std::move(entry));
    }
    return out;
}

ToolView decode_tool_view(const json& view) {
    if (!view.is_object()) return std::monostate{};
    const std::string kind = text_field(view, "kind");
    if (kind == "read") {
        ReadView out;
        out.path = text_field(view, "path");
        out.start_line = view.value("start_line", 0);
        out.end_line = view.value("end_line", 0);
        out.directory = view.value("directory", false);
        return out;
    }
    if (kind == "change") {
        FileChangeView out;
        out.path = text_field(view, "path");
        out.diff = text_field(view, "diff");
        out.added = view.value("added", 0);
        out.removed = view.value("removed", 0);
        out.created = view.value("created", false);
        return out;
    }
    if (kind == "bash") {
        BashView out;
        out.command = text_field(view, "command");
        out.output = text_field(view, "output");
        out.interrupted = view.value("interrupted", false);
        out.timed_out = view.value("timed_out", false);
        if (const auto it = view.find("exit_code"); it != view.end() && it->is_number_integer())
            out.exit_code = it->get<int>();
        if (const auto it = view.find("signal"); it != view.end() && it->is_number_integer())
            out.signal = it->get<int>();
        out.elapsed_ms = view.value("elapsed_ms", 0);
        return out;
    }
    if (kind == "skill") return SkillView{text_field(view, "name"), text_field(view, "path")};
    if (kind == "grep") {
        GrepView out;
        out.pattern = text_field(view, "pattern");
        for (const json& line : view.value("lines", json::array())) {
            out.lines.push_back({text_field(line, "path"), text_field(line, "text"), line.value("line", 0)});
        }
        return out;
    }
    if (kind == "glob") {
        GlobView out;
        out.pattern = text_field(view, "pattern");
        out.files = view.value("files", std::vector<std::string>{});
        return out;
    }
    if (kind == "web") {
        WebView out;
        out.operation = view.at("operation").get<std::string>();
        out.query = view.at("query").get<std::string>();
        out.url = view.at("url").get<std::string>();
        out.title = view.at("title").get<std::string>();
        out.output = view.at("output").get<std::string>();
        out.status = view.at("status").get<int>();
        out.offset = view.at("offset").get<std::size_t>();
        out.next_offset = view.at("next_offset").get<std::size_t>();
        out.total_bytes = view.at("total_bytes").get<std::size_t>();
        out.cached = view.at("cached").get<bool>();
        out.truncated = view.at("truncated").get<bool>();
        out.elapsed_ms = view.at("elapsed_ms").get<std::int64_t>();
        return out;
    }
    if (kind == "mcp") {
        McpView out;
        out.server = text_field(view, "server");
        out.tool = text_field(view, "tool");
        out.content = view.value("content", std::vector<json>{});
        return out;
    }
    if (kind == "todo") return todo_from(view.value("items", json::array()));
    if (kind == "ask") {
        AskView out;
        out.header = text_field(view, "header");
        out.prompt = text_field(view, "prompt");
        for (const json& option : view.value("options", json::array())) {
            out.options.push_back({text_field(option, "label"), text_field(option, "description")});
        }
        out.selected = view.value("selected", std::vector<int>{});
        out.other = text_field(view, "other");
        out.cancelled = view.value("cancelled", false);
        return out;
    }
    if (kind == "task") {
        TaskView out;
        out.agent = text_field(view, "agent");
        out.session_id = text_field(view, "session_id");
        out.result = text_field(view, "result");
        for (const json& step : view.value("steps", json::array())) {
            out.steps.push_back({text_field(step, "summary"), step.value("is_error", false)});
        }
        out.tool_calls = view.value("tool_calls", 0);
        out.seconds = view.value("seconds", 0.0);
        return out;
    }
    return std::monostate{};
}

std::optional<EventPayload> decode_payload(std::string_view kind, const json& data) {
    if (kind == "turn_started") return TurnStarted{data.value("input", "")};
    if (kind == "step_started") return StepStarted{};
    if (kind == "text") return TextDelta{data.value("text", "")};
    if (kind == "reasoning") return ReasoningDelta{data.value("text", "")};
    if (kind == "stream_reset") return StreamReset{};
    if (kind == "tool_pending") return ToolPending{data.value("name", "")};
    if (kind == "tool_started")
        return ToolStarted{data.value("id", ""), data.value("name", ""), data.value("summary", "")};
    if (kind == "tool_output") return ToolOutput{data.value("id", ""), data.value("chunk", "")};
    if (kind == "tool_finished") {
        ToolFinished out;
        out.id = data.value("id", "");
        out.name = data.value("name", "");
        out.summary = data.value("summary", "");
        out.text = data.value("text", "");
        out.is_error = data.value("is_error", false);
        out.interrupted = data.value("interrupted", false);
        out.view = decode_tool_view(data.value("view", json::object()));
        return out;
    }
    if (kind == "retrying") {
        Retrying out;
        out.attempt = data.value("attempt", 0);
        out.max_attempts = data.value("max_attempts", 0);
        out.wait_ms = data.value("wait_ms", std::int64_t{0});
        out.reason = data.value("reason", "");
        return out;
    }
    if (kind == "compacted") {
        Compacted out;
        out.before = data.value("before", std::size_t{0});
        out.after = data.value("after", std::size_t{0});
        return out;
    }
    if (kind == "context") {
        ContextUpdate out;
        out.used = data.value("used", std::size_t{0});
        out.limit = data.value("limit", std::size_t{0});
        return out;
    }
    if (kind == "notice") {
        Notice out;
        const std::string level = data.value("level", "info");
        out.level = level == "warn" ? NoticeLevel::warn
                  : level == "error" ? NoticeLevel::error
                                     : NoticeLevel::info;
        out.text = data.value("text", "");
        out.persistent = data.value("persistent", false);
        return out;
    }
    if (kind == "model_changed") return ModelChanged{data.value("model", "")};
    if (kind == "mode_changed")
        return ModeChanged{data.value("mode", ""), data.value("planning", false)};
    if (kind == "turn_ended") {
        TurnEnded out;
        out.status = status_from(data.value("status", "done"));
        out.error = data.value("error", "");
        return out;
    }
    return std::nullopt;
}

} // namespace

std::optional<Event> decode_event(const protocol::Event& event) {
    std::optional<EventPayload> payload = decode_payload(event.kind, event.data);
    if (!payload) return std::nullopt;
    return Event{std::move(*payload)};
}

std::vector<Event> decode_history(const protocol::HistoryItem& item) {
    std::vector<Event> events;
    if (item.kind == "system") {
        if (!item.model.empty()) events.push_back(Event{ModelChanged{item.model}});
    } else if (item.kind == "user") {
        events.push_back(Event{TurnStarted{item.text}});
    } else if (item.kind == "assistant") {
        if (!item.reasoning.empty()) events.push_back(Event{ReasoningDelta{item.reasoning}});
        if (!item.text.empty()) events.push_back(Event{TextDelta{item.text}});
    } else if (item.kind == "tool_started") {
        events.push_back(Event{ToolStarted{item.started.value("id", ""),
                                          item.started.value("name", ""),
                                          item.started.value("summary", "")}});
    } else if (item.kind == "permission" || item.kind == "parent_review") {
        events.push_back(Event{Notice{NoticeLevel::info, item.text, true}});
    } else if (item.kind == "tool") {
        ToolFinished out;
        out.id = item.call_id;
        out.name = item.name;
        out.summary = item.summary;
        out.text = item.result.value("text", "");
        out.is_error = item.result.value("is_error", false);
        out.interrupted = item.result.value("interrupted", false);
        out.view = decode_tool_view(item.result.value("view", json::object()));
        events.push_back(Event{std::move(out)});

    }
    return events;
}

ApprovalRequest decode_approval(const json& payload) {
    ApprovalRequest out;
    out.tool = payload.value("tool", "");
    out.agent = payload.value("agent", "");
    out.reason = payload.value("reason", "");
    out.summary = payload.value("summary", "");
    out.preview_kind = payload.value("preview_kind", "text");
    out.preview_text = payload.value("preview_text", "");
    out.cwd = payload.value("cwd", "");
    out.mode = payload.value("mode", "");
    out.session_rule = payload.value("session_rule", "");
    out.partially_executed = payload.value("partially_executed", false);
    for (const json& request : payload.value("requests", json::array())) {
        out.requests.push_back({request.value("kind", ""), request.value("target", ""),
                                request.value("reason", "")});
    }
    return out;
}

QuestionRequest decode_question(const json& payload) {
    QuestionRequest out;
    out.header = payload.value("header", "");
    out.prompt = payload.value("prompt", "");
    out.multi_select = payload.value("multi_select", false);
    out.allow_other = payload.value("allow_other", true);
    for (const json& option : payload.value("options", json::array())) {
        out.options.push_back({option.value("label", ""), option.value("description", "")});
    }
    return out;
}

std::vector<McpStatus> decode_mcp(const json& array) {
    std::vector<McpStatus> out;
    if (!array.is_array()) return out;
    for (const json& item : array) {
        out.push_back({item.value("name", ""), item.value("status", ""), item.value("error", ""),
                       item.value("boundary", ""), item.value("tools", std::size_t{0})});
    }
    return out;
}

} // namespace dagent::ui
