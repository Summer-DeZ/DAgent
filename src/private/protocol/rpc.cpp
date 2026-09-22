#include "protocol/rpc.hpp"

#include <utility>

namespace dagent::protocol {
namespace {

nlohmann::json envelope(const char* kind_key, std::string name, nlohmann::json body) {
    nlohmann::json json{{"jsonrpc", "2.0"}};
    json[kind_key] = std::move(name);
    for (auto& [key, value] : body.items()) json[key] = std::move(value);
    return json;
}

} // namespace

std::string encode_request(const Request& request) {
    nlohmann::json params = request.params.is_null() ? nlohmann::json::object() : request.params;
    return envelope("method", request.method,
                    nlohmann::json{{"id", request.id}, {"params", std::move(params)}})
        .dump();
}

std::string encode_result(const std::string& id, nlohmann::json result) {
    return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}.dump();
}

std::string encode_error(const std::string& id, const RpcError& error) {
    nlohmann::json data = error.data.is_object() ? error.data : nlohmann::json::object();
    if (!error.kind.empty()) data["kind"] = error.kind;
    nlohmann::json object{{"jsonrpc", "2.0"},
                          {"id", id},
                          {"error", {{"code", error.code}, {"message", error.message}, {"data", data}}}};
    return object.dump();
}

std::string encode_notification(const Notification& notification) {
    nlohmann::json params = notification.params.is_null() ? nlohmann::json::object() : notification.params;
    return envelope("method", notification.method, nlohmann::json{{"params", std::move(params)}}).dump();
}

Message parse_message(std::string_view line) {
    Message message;
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception& error) {
        message.kind = Message::Kind::invalid;
        message.error = std::string("invalid JSON: ") + error.what();
        return message;
    }
    if (!json.is_object()) {
        message.kind = Message::Kind::invalid;
        message.error = "message must be a JSON object";
        return message;
    }
    if (json.value("jsonrpc", "") != "2.0") {
        message.kind = Message::Kind::invalid;
        message.error = "jsonrpc must be \"2.0\"";
        return message;
    }
    const bool has_method = json.contains("method");
    const bool has_id = json.contains("id") && !json["id"].is_null();
    if (has_method) {
        if (!json["method"].is_string()) {
            message.kind = Message::Kind::invalid;
            message.error = "method must be a string";
            return message;
        }
        const auto params = json.find("params");
        nlohmann::json parsed = params == json.end() || params->is_null() ? nlohmann::json::object()
                                                                         : *params;
        if (!parsed.is_object() && !parsed.is_array()) {
            message.kind = Message::Kind::invalid;
            message.error = "params must be an object or array";
            return message;
        }
        if (has_id) {
            message.kind = Message::Kind::request;
            message.request.id = json["id"].is_string() ? json["id"].get<std::string>() : json["id"].dump();
            message.request.method = json["method"].get<std::string>();
            message.request.params = std::move(parsed);
        } else {
            message.kind = Message::Kind::notification;
            message.notification.method = json["method"].get<std::string>();
            message.notification.params = std::move(parsed);
        }
        return message;
    }
    if (has_id) {
        message.kind = Message::Kind::response;
        message.response.id = json["id"].is_string() ? json["id"].get<std::string>() : json["id"].dump();
        if (const auto error = json.find("error"); error != json.end() && error->is_object()) {
            RpcError parsed;
            parsed.code = error->value("code", rpc_code::kInternalError);
            parsed.message = error->value("message", "");
            if (const auto data = error->find("data"); data != error->end() && data->is_object()) {
                parsed.data = *data;
                parsed.kind = data->value("kind", "");
            }
            message.response.error = std::move(parsed);
        } else {
            message.response.result = json.value("result", nlohmann::json(nullptr));
        }
        return message;
    }
    message.kind = Message::Kind::invalid;
    message.error = "message has neither method nor id";
    return message;
}

} // namespace dagent::protocol
