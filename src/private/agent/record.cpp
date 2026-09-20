#include "agent/record.hpp"
#include "agent/compaction.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <utility>

#include "base/log.hpp"
#include "base/text.hpp"

namespace dagent::agent {
namespace {

using nlohmann::json;

std::shared_ptr<spdlog::logger> log_agent() { return base::logger("agent"); }

const char* answer_name(Decision::Answer answer) {
    switch (answer) {
    case Decision::Answer::allow: return "allow";
    case Decision::Answer::allow_session: return "allow_session";
    case Decision::Answer::deny: return "deny";
    case Decision::Answer::deny_with_feedback: return "deny_with_feedback";
    }
    return "deny";
}

json usage_json(const Usage& usage) {
    return json{{"prompt", usage.prompt}, {"completion", usage.completion}, {"cached", usage.cached}};
}

[[noreturn]] void corrupt(std::string_view type, std::string_view message) {
    throw session::SessionError(session::SessionError::Kind::corrupt,
                                std::format("corrupt session record {}: {}", type, message));
}

void require_object(std::string_view type, const json& payload) {
    if (!payload.is_object()) corrupt(type, "missing payload; record_payloads may have been disabled");
}

std::string string_field(std::string_view type, const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) corrupt(type, std::format("field {} must be a string", key));
    return it->get<std::string>();
}

std::int64_t integer_field(std::string_view type, const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || (!it->is_number_integer() && !it->is_number_unsigned())) {
        corrupt(type, std::format("field {} must be an integer", key));
    }
    try {
        return it->get<std::int64_t>();
    } catch (const json::exception&) {
        corrupt(type, std::format("field {} is out of range", key));
    }
}

bool bool_field(std::string_view type, const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_boolean()) corrupt(type, std::format("field {} must be a boolean", key));
    return it->get<bool>();
}

Usage parse_usage(std::string_view type, const json& value) {
    if (!value.is_object()) corrupt(type, "field usage must be an object");
    Usage usage;
    usage.prompt = integer_field(type, value, "prompt");
    usage.completion = integer_field(type, value, "completion");
    usage.cached = integer_field(type, value, "cached");
    return usage;
}

TurnStatus parse_status(std::string_view value) {
    if (value == "done") return TurnStatus::done;
    if (value == "interrupted") return TurnStatus::interrupted;
    if (value == "denied") return TurnStatus::denied;
    if (value == "limit") return TurnStatus::limit;
    if (value == "failed" || value == "crashed") return TurnStatus::failed;
    corrupt("turn_end", std::format("unknown status {}", value));
}

std::size_t utf8_prefix_chars(std::string_view text, std::size_t characters) {
    std::size_t pos = 0;
    for (std::size_t count = 0; pos < text.size() && count < characters; ++count) {
        const auto lead = static_cast<unsigned char>(text[pos]);
        pos += lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    }
    return std::min(pos, text.size());
}

} // namespace

Recorder Recorder::create(const session::Options& options, session::Meta meta) {
    return Recorder(session::Writer::create(options, std::move(meta)));
}

Recorder Recorder::resume(const session::Options& options, std::string_view id) {
    return Recorder(session::Writer::resume(options, id));
}

void Recorder::append(std::string_view type, nlohmann::json payload) {
    if (broken_ || !writer_) return;
    try {
        writer_->append(type, std::move(payload));
    } catch (const session::SessionError& e) {
        broken_ = true;
        error_ = e.what();
        log_agent()->error("failed to write session record: {}", e.what());
    }
}

void Recorder::system(std::string_view text) {
    append("system", json{{"schema", 1}, {"text", text}});
}

void Recorder::user(std::int64_t n, std::string_view text) {
    append("user", json{{"n", n}, {"text", text}});
}

void Recorder::assistant(std::int64_t n, const Reply& reply) {
    json tool_calls = json::array();
    for (const ToolCall& call : reply.message.tool_calls) {
        tool_calls.push_back({{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}});
    }
    json payload = {{"n", n},
                    {"content", reply.message.content},
                    {"reasoning", reply.message.reasoning_content},
                    {"tool_calls", std::move(tool_calls)},
                    {"finish", reply.finish.raw}};
    if (reply.usage) payload["usage"] = usage_json(*reply.usage);
    append("assistant", std::move(payload));
}

void Recorder::tool(std::int64_t n, const ToolCall& call, std::string_view summary,
                    const tools::Result& result) {
    append("tool", json{{"n", n},
                        {"call_id", call.id},
                        {"name", call.name},
                        {"summary", summary},
                        {"text", result.text},
                        {"is_error", result.is_error},
                        {"interrupted", result.interrupted},
                        {"view", tools::to_json(result.display)}});
}

void Recorder::permission(std::string_view call_id, const Decision& decision, std::string_view rule) {
    append("permission", json{{"call_id", call_id},
                              {"answer", answer_name(decision.answer)},
                              {"rule", rule},
                              {"network", decision.network}});
}

void Recorder::turn_end(TurnStatus status, std::string_view error, int steps, int tool_calls,
                        const Usage& total) {
    append("turn_end", json{{"status", to_string(status)},
                            {"error", error},
                            {"steps", steps},
                            {"tool_calls", tool_calls},
                            {"usage", usage_json(total)}});
}

void Recorder::prune(const std::vector<std::int64_t>& ordinals) {
    append("prune", json{{"ordinals", ordinals}});
}

void Recorder::compaction(std::int64_t keep_from, std::string_view summary) {
    append("compaction", json{{"keep_from", keep_from}, {"summary", summary}});
}

void Recorder::turn_end_crashed() {
    append("turn_end", json{{"status", "crashed"},
                            {"error", ""},
                            {"steps", 0},
                            {"tool_calls", 0},
                            {"usage", usage_json(Usage{})}});
}

void Recorder::sync() {
    if (broken_ || !writer_) return;
    try {
        writer_->sync();
    } catch (const session::SessionError& e) {
        broken_ = true;
        error_ = e.what();
        log_agent()->error("failed to flush session record: {}", e.what());
    }
}

const session::Meta& Recorder::meta() const { return writer_->meta(); }

Restored replay_into(const session::Options& options, std::string_view id, const Sink& sink) {
    Conversation conversation;
    bool saw_system = false;
    bool open_turn = false;
    std::int64_t next_ordinal = 0;

    session::replay(options, id, [&](std::string_view type, const json& payload) {
        require_object(type, payload);
        if (!saw_system && type != "system") corrupt(type, "first core record is not system");

        if (type == "system") {
            const std::int64_t schema = integer_field(type, payload, "schema");
            if (schema != 1) corrupt(type, std::format("unknown session format version {}", schema));
            (void)string_field(type, payload, "text");
            saw_system = true;
            return;
        }
        if (type == "user") {
            const std::int64_t n = integer_field(type, payload, "n");
            if (n != next_ordinal) corrupt(type, std::format("expected message ordinal {}, got {}", next_ordinal, n));
            std::string text = string_field(type, payload, "text");
            Entry entry;
            entry.message.role = Role::user;
            entry.message.content = text;
            entry.ordinal = n;
            conversation.restore(std::move(entry));
            ++next_ordinal;
            open_turn = true;
            sink(TurnStarted{std::move(text)});
            return;
        }
        if (type == "assistant") {
            const std::int64_t n = integer_field(type, payload, "n");
            if (n != next_ordinal) corrupt(type, std::format("expected message ordinal {}, got {}", next_ordinal, n));
            Message message;
            message.role = Role::assistant;
            message.content = string_field(type, payload, "content");
            message.reasoning_content = string_field(type, payload, "reasoning");
            const auto calls = payload.find("tool_calls");
            if (calls == payload.end() || !calls->is_array()) corrupt(type, "field tool_calls must be an array");
            for (const json& item : *calls) {
                if (!item.is_object()) corrupt(type, "tool_calls entries must be objects");
                message.tool_calls.push_back(ToolCall{string_field(type, item, "id"),
                                                      string_field(type, item, "name"),
                                                      string_field(type, item, "arguments")});
            }
            (void)string_field(type, payload, "finish");
            if (const auto usage = payload.find("usage"); usage != payload.end()) {
                (void)parse_usage(type, *usage);
            }
            if (!message.reasoning_content.empty()) sink(ReasoningDelta{message.reasoning_content});
            if (!message.content.empty()) sink(TextDelta{message.content});
            Entry entry;
            entry.message = std::move(message);
            entry.ordinal = n;
            conversation.restore(std::move(entry));
            ++next_ordinal;
            return;
        }
        if (type == "tool") {
            const std::int64_t n = integer_field(type, payload, "n");
            if (n != next_ordinal) corrupt(type, std::format("expected message ordinal {}, got {}", next_ordinal, n));
            const std::string call_id = string_field(type, payload, "call_id");
            const std::string name = string_field(type, payload, "name");
            const std::string summary = string_field(type, payload, "summary");
            const std::string text = string_field(type, payload, "text");
            tools::Result result;
            result.text = text;
            result.is_error = bool_field(type, payload, "is_error");
            result.interrupted = bool_field(type, payload, "interrupted");
            const auto view = payload.find("view");
            if (view == payload.end() || !view->is_object()) corrupt(type, "field view must be an object");
            result.display = tools::view_from_json(*view);

            Entry entry;
            entry.message.role = Role::tool;
            entry.message.tool_call_id = call_id;
            entry.message.content = text;
            entry.ordinal = n;
            entry.summary = summary;
            conversation.restore(std::move(entry));
            ++next_ordinal;
            sink(ToolFinished{call_id, name, summary, std::move(result)});
            return;
        }
        if (type == "prune") {
            const auto ordinals = payload.find("ordinals");
            if (ordinals == payload.end() || !ordinals->is_array()) corrupt(type, "field ordinals must be an array");
            for (const auto& value : *ordinals) {
                if (!value.is_number_integer()) corrupt(type, "message ordinal must be an integer");
                const auto n = value.get<std::int64_t>();
                const auto& entries = conversation.entries();
                const auto found = std::find_if(entries.begin(), entries.end(),
                                                [n](const Entry& e) { return e.ordinal == n; });
                if (found == entries.end() || found->message.role != Role::tool) {
                    corrupt(type, "pruned ordinal has no matching tool message");
                }
                conversation.prune(static_cast<std::size_t>(found - entries.begin()),
                                   texts::pruned_output(found->summary));
            }
            return;
        }
        if (type == "compaction") {
            const auto keep_from = integer_field(type, payload, "keep_from");
            const auto summary = string_field(type, payload, "summary");
            const auto& entries = conversation.entries();
            const auto found = std::find_if(entries.begin(), entries.end(),
                                            [keep_from](const Entry& e) { return e.ordinal == keep_from; });
            if (keep_from < 0 || found == entries.end()) corrupt(type, "summary cut does not exist");
            const auto cut = static_cast<std::size_t>(found - entries.begin());
            const auto cuts = conversation.safe_cuts();
            if (std::find(cuts.begin(), cuts.end(), cut) == cuts.end()) corrupt(type, "summary cut is unsafe");
            if (summary.empty()) conversation.discard_prefix(cut);
            else conversation.replace_prefix(cut, texts::summary_message(summary));
            if (const auto invalid = conversation.validate()) corrupt(type, *invalid);
            return;
        }
        if (type == "permission") {
            (void)string_field(type, payload, "call_id");
            (void)string_field(type, payload, "answer");
            (void)string_field(type, payload, "rule");
            (void)bool_field(type, payload, "network");
            return;
        }
        if (type == "turn_end") {
            const std::string status_name = string_field(type, payload, "status");
            const std::string stored_error = string_field(type, payload, "error");
            const std::int64_t steps = integer_field(type, payload, "steps");
            const std::int64_t tool_calls = integer_field(type, payload, "tool_calls");
            const auto usage = payload.find("usage");
            if (usage == payload.end()) corrupt(type, "missing field usage");
            const Usage total = parse_usage(type, *usage);
            const TurnStatus status = parse_status(status_name);
            const std::string error = status_name == "crashed" && stored_error.empty()
                                          ? "session unexpectedly interrupted"
                                          : stored_error;
            open_turn = false;
            sink(TurnEnded{status, error, static_cast<int>(steps), static_cast<int>(tool_calls), total});
            return;
        }
        corrupt(type, "unknown record type");
    });

    if (!saw_system) {
        throw session::SessionError(session::SessionError::Kind::corrupt,
                                    "session record has no system entry; record_payloads may have been disabled");
    }
    conversation.set_next_ordinal(next_ordinal);

    // 未闭合历史只允许缺少最后一批工具结果。先在内存副本上投影崩溃闭合并验证，
    // 确认记录前缀本身合法后，Agent::resume 才会打开 Writer 把 T9 写回 JSONL。
    const std::vector<ToolCall> open_calls = open_turn ? conversation.open_calls()
                                                       : std::vector<ToolCall>{};
    Conversation checked = conversation;
    for (const ToolCall& call : open_calls) {
        checked.add_tool_result(call.id, std::string(texts::kCrashed), "recover interrupted call");
    }
    if (const std::optional<std::string> invalid = checked.validate()) {
        throw session::SessionError(session::SessionError::Kind::corrupt,
                                    "inconsistent session history: " + *invalid);
    }

    Restored restored;
    restored.conversation = std::move(conversation);
    restored.unfinished = open_turn;
    restored.open_calls = open_calls;
    return restored;
}

std::string session_title(const nlohmann::json& first_events) {
    if (!first_events.is_array()) return {};
    for (const json& event : first_events) {
        if (!event.is_object() || event.value("type", "") != "user") continue;
        const auto payload = event.find("payload");
        if (payload == event.end() || !payload->is_object()) return {};
        const auto text = payload->find("text");
        if (text == payload->end() || !text->is_string()) return {};
        std::string title = base::to_valid_utf8(text->get<std::string>());
        if (const std::size_t newline = title.find_first_of("\r\n"); newline != std::string::npos) {
            title.resize(newline);
        }
        title.resize(utf8_prefix_chars(title, 60));
        return title;
    }
    return {};
}

std::string resolve_session_id(const session::Options& options,
                               const std::filesystem::path& project_root,
                               std::optional<std::string_view> prefix) {
    const std::vector<session::Summary> sessions =
        session::list(options, project_root, 0, session_title);
    if (!prefix) {
        if (sessions.empty()) throw std::runtime_error("no sessions for this project");
        return sessions.front().meta.id;
    }

    std::vector<const session::Summary*> matches;
    for (const session::Summary& summary : sessions) {
        if (summary.meta.id == *prefix) return summary.meta.id;
        if (summary.meta.id.starts_with(*prefix)) matches.push_back(&summary);
    }
    if (matches.empty()) throw std::runtime_error("session not found in this project: " + std::string(*prefix));
    if (matches.size() == 1) return matches.front()->meta.id;

    std::string message = "ambiguous session ID prefix; use a longer prefix: ";
    for (const session::Summary* match : matches) {
        message += "\n  " + match->meta.id;
        if (!match->title.empty()) message += "  " + match->title;
    }
    throw std::runtime_error(message);
}

} // namespace dagent::agent
