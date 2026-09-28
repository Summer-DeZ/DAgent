#include "protocol/dto.hpp"

#include <utility>

namespace dagent::protocol {
namespace {

nlohmann::json optional_string(const std::optional<std::string>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

std::optional<std::string> read_optional_string(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return std::nullopt;
    return it->get<std::string>();
}

} // namespace

void to_json(nlohmann::json& json, const PublicModel& model) {
    json = nlohmann::json{{"name", model.name},           {"kind", model.kind},
                          {"model", model.model},         {"base_url", model.base_url},
                          {"max_tokens", model.max_tokens},
                          {"temperature", model.temperature},
                          {"context_window", model.context_window},
                          {"has_key", model.has_key}};
}

void from_json(const nlohmann::json& json, PublicModel& model) {
    model.name = json.value("name", "");
    model.kind = json.value("kind", "");
    model.model = json.value("model", "");
    model.base_url = json.value("base_url", "");
    model.max_tokens = json.value("max_tokens", std::size_t{0});
    model.temperature = json.value("temperature", -1.0);
    model.context_window = json.value("context_window", std::size_t{0});
    model.has_key = json.value("has_key", false);
}

void to_json(nlohmann::json& json, const ProviderKind& kind) {
    json = nlohmann::json{{"kind", kind.kind},
                          {"default_base_url", kind.default_base_url},
                          {"needs_credential", kind.needs_credential}};
}

void from_json(const nlohmann::json& json, ProviderKind& kind) {
    kind.kind = json.value("kind", "");
    kind.default_base_url = json.value("default_base_url", "");
    kind.needs_credential = json.value("needs_credential", false);
}

void to_json(nlohmann::json& json, const Usage& usage) {
    json = nlohmann::json{{"prompt", usage.prompt}, {"completion", usage.completion}, {"cached", usage.cached}};
}

void from_json(const nlohmann::json& json, Usage& usage) {
    usage.prompt = json.value("prompt", std::int64_t{0});
    usage.completion = json.value("completion", std::int64_t{0});
    usage.cached = json.value("cached", std::int64_t{0});
}

void to_json(nlohmann::json& json, const QueueItem& item) {
    json = nlohmann::json{{"input_id", item.input_id}, {"text_preview", item.text_preview}};
}

void from_json(const nlohmann::json& json, QueueItem& item) {
    item.input_id = json.value("input_id", "");
    item.text_preview = json.value("text_preview", "");
}

void to_json(nlohmann::json& json, const OperationInfo& info) {
    json = nlohmann::json{{"kind", info.kind}, {"run_id", optional_string(info.run_id.empty()
                                                                               ? std::nullopt
                                                                               : std::optional<std::string>(info.run_id))}};
}

void from_json(const nlohmann::json& json, OperationInfo& info) {
    info.kind = json.value("kind", "");
    info.run_id = read_optional_string(json, "run_id").value_or("");
}

void to_json(nlohmann::json& json, const ContextInfo& info) {
    json = nlohmann::json{
        {"used", info.used}, {"limit", info.limit}, {"window", info.window}, {"trigger_percent", info.trigger_percent}};
}

void from_json(const nlohmann::json& json, ContextInfo& info) {
    info.used = json.value("used", std::size_t{0});
    info.limit = json.value("limit", std::size_t{0});
    info.window = json.value("window", std::size_t{0});
    info.trigger_percent = json.at("trigger_percent").get<int>();
}

void to_json(nlohmann::json& json, const RecordingInfo& info) {
    json = nlohmann::json{{"broken", info.broken}, {"error", info.error}};
}

void from_json(const nlohmann::json& json, RecordingInfo& info) {
    info.broken = json.value("broken", false);
    info.error = json.value("error", "");
}

void to_json(nlohmann::json& json, const SessionSnapshot& snapshot) {
    json = nlohmann::json{
        {"session_id", snapshot.session_id},
        {"session_generation", snapshot.session_generation},
        {"state_seq", snapshot.state_seq},
        {"model", snapshot.model},
        {"permission_mode", snapshot.permission_mode},
        {"planning", snapshot.planning},
        {"read_only", snapshot.read_only},
        {"busy", snapshot.busy},
        {"current_operation", snapshot.current_operation ? nlohmann::json(*snapshot.current_operation)
                                                         : nlohmann::json(nullptr)},
        {"queue", snapshot.queue},
        {"context", snapshot.context},
        {"work_plan", snapshot.work_plan},
        {"mcp", snapshot.mcp},
        {"recording", snapshot.recording},
    };
}

void from_json(const nlohmann::json& json, SessionSnapshot& snapshot) {
    snapshot.session_id = json.value("session_id", "");
    snapshot.session_generation = json.value("session_generation", std::uint64_t{0});
    snapshot.state_seq = json.value("state_seq", std::uint64_t{0});
    snapshot.model = json.value("model", PublicModel{});
    snapshot.permission_mode = json.value("permission_mode", "workspace");
    snapshot.planning = json.value("planning", false);
    snapshot.read_only = json.value("read_only", false);
    snapshot.busy = json.value("busy", false);
    if (const auto it = json.find("current_operation"); it != json.end() && !it->is_null()) {
        snapshot.current_operation = it->get<OperationInfo>();
    }
    snapshot.queue = json.value("queue", std::vector<QueueItem>{});
    snapshot.context = json.value("context", ContextInfo{});
    snapshot.work_plan = json.value("work_plan", nlohmann::json::array());
    snapshot.mcp = json.value("mcp", nlohmann::json::array());
    snapshot.recording = json.value("recording", RecordingInfo{});
}

void to_json(nlohmann::json& json, const HistoryItem& item) {
    json = nlohmann::json{{"kind", item.kind},        {"seq", item.seq},
                          {"text", item.text},        {"reasoning", item.reasoning},
                          {"finish", item.finish},    {"model", item.model},
                          {"call_id", item.call_id},  {"name", item.name},
                          {"summary", item.summary},  {"result", item.result},
                          {"started", item.started},  {"status", item.status},
                          {"error", item.error},      {"steps", item.steps},
                          {"tool_calls", item.tool_calls}, {"usage", item.usage}};
}

void from_json(const nlohmann::json& json, HistoryItem& item) {
    item.kind = json.value("kind", "user");
    item.seq = json.value("seq", std::int64_t{-1});
    item.text = json.value("text", "");
    item.reasoning = json.value("reasoning", "");
    item.finish = json.value("finish", "");
    item.model = json.value("model", "");
    item.call_id = json.value("call_id", "");
    item.name = json.value("name", "");
    item.summary = json.value("summary", "");
    item.result = json.value("result", nlohmann::json(nullptr));
    item.started = json.value("started", nlohmann::json(nullptr));
    item.status = json.value("status", "done");
    item.error = json.value("error", "");
    item.steps = json.value("steps", 0);
    item.tool_calls = json.value("tool_calls", 0);
    item.usage = json.value("usage", nlohmann::json::object());
}

void to_json(nlohmann::json& json, const Event& event) {
    json = nlohmann::json{{"seq", event.seq},
                          {"session_id", event.session_id},
                          {"session_generation", event.session_generation},
                          {"kind", event.kind},
                          {"data", event.data},
                          {"parent_session_id", optional_string(event.parent_session_id)},
                          {"model_call_id", optional_string(event.model_call_id)},
                          {"agent", optional_string(event.agent)}};
}

void from_json(const nlohmann::json& json, Event& event) {
    event.seq = json.value("seq", std::uint64_t{0});
    event.session_id = json.value("session_id", "");
    event.session_generation = json.value("session_generation", std::uint64_t{0});
    event.kind = json.value("kind", "");
    event.data = json.value("data", nlohmann::json::object());
    event.parent_session_id = read_optional_string(json, "parent_session_id");
    event.model_call_id = read_optional_string(json, "model_call_id");
    event.agent = read_optional_string(json, "agent");
}

void to_json(nlohmann::json& json, const InteractionRequest& request) {
    json = nlohmann::json{{"interaction_id", request.interaction_id},
                          {"kind", request.kind},
                          {"session_id", request.session_id},
                          {"session_generation", request.session_generation},
                          {"payload", request.payload}};
}

void from_json(const nlohmann::json& json, InteractionRequest& request) {
    request.interaction_id = json.value("interaction_id", "");
    request.kind = json.value("kind", "approval");
    request.session_id = json.value("session_id", "");
    request.session_generation = json.value("session_generation", std::uint64_t{0});
    request.payload = json.value("payload", nlohmann::json::object());
}

} // namespace dagent::protocol
