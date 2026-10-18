#include "llm/decision_systemone.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "base/log.hpp"
#include "base/text.hpp"
#include "lib/nlohmann/json.hpp"

namespace dagent::llm {
namespace {

using nlohmann::json;

constexpr std::string_view kAdapterVersion = "1";

std::shared_ptr<spdlog::logger> log_llm() { return base::logger("llm"); }

const char* direction_name(agent::Direction direction) {
    switch (direction) {
    case agent::Direction::fetch: return "fetch";
    case agent::Direction::modify: return "modify";
    case agent::Direction::send: return "send";
    case agent::Direction::unknown: return "unknown";
    }
    return "unknown";
}

std::optional<agent::Direction> direction_from(std::string_view name) {
    if (name == "fetch") return agent::Direction::fetch;
    if (name == "modify") return agent::Direction::modify;
    if (name == "send") return agent::Direction::send;
    if (name == "unknown") return agent::Direction::unknown;
    return std::nullopt;
}

std::string dimension_name(agent::Dimension dimension) {
    return std::string(agent::to_string(dimension));
}

/// @brief System One 客户端：state + questions → answers。<键>.<noul|choice>。
/// 服务只有一个处理槽，内部用一个 HttpClient 串行发送；请求可被 stop / deadline 取消。
class SystemOneDecisionModel final : public agent::DecisionModel {
public:
    SystemOneDecisionModel(DecisionProviderConfig config, std::chrono::milliseconds timeout,
                           net::HttpOptions http)
        : config_(std::move(config)), timeout_(timeout), client_(std::move(http)) {}

    std::expected<agent::DecisionReply, agent::DecisionError> evaluate(
        const agent::DecisionRequest& request, agent::ReviewDeadline deadline,
        std::stop_token stop) override {
        if (config_.endpoint.empty())
            return std::unexpected(agent::DecisionError{
                agent::DecisionError::Kind::unavailable, "decision model endpoint is empty"});

        const auto local_deadline = std::min(deadline, std::chrono::steady_clock::now() + timeout_);
        std::unique_lock lock(mutex_);
        if (!available_.wait_until(lock, stop, local_deadline, [&] { return !busy_; }))
            return std::unexpected(agent::DecisionError{
                stop.stop_requested() ? agent::DecisionError::Kind::cancelled : agent::DecisionError::Kind::timeout,
                "decision request cancelled or timed out while queued"});
        if (stop.stop_requested())
            return std::unexpected(agent::DecisionError{agent::DecisionError::Kind::cancelled,
                                                        "decision request cancelled while queued"});
        busy_ = true;
        lock.unlock();
        struct Release {
            SystemOneDecisionModel& owner;
            ~Release() {
                { const std::lock_guard lock(owner.mutex_); owner.busy_ = false; }
                owner.available_.notify_all();
            }
        } release{*this};
        net::HttpRequest http_request;
        http_request.url = config_.endpoint;
        http_request.method = "POST";
        http_request.headers.emplace_back("Content-Type", "application/json");
        http_request.body = encode(request).dump();

        net::HttpResponse response;
        try {
            response = client_.send(http_request, stop, local_deadline);
        } catch (const net::HttpError& error) {
            const auto kind = error.kind() == net::HttpError::Kind::cancelled
                                  ? agent::DecisionError::Kind::cancelled
                              : error.kind() == net::HttpError::Kind::timeout
                                  ? agent::DecisionError::Kind::timeout
                                  : agent::DecisionError::Kind::unavailable;
            return std::unexpected(agent::DecisionError{kind, error.what()});
        }
        if (response.status < 200 || response.status >= 300)
            return std::unexpected(agent::DecisionError{
                agent::DecisionError::Kind::unavailable,
                std::format("decision service returned HTTP {}", response.status)});

        json body;
        try {
            body = json::parse(response.body);
        } catch (const json::exception& error) {
            return std::unexpected(agent::DecisionError{agent::DecisionError::Kind::invalid_response,
                                                        std::format("invalid decision response: {}", error.what())});
        }
        try {
            agent::DecisionReply reply;
            reply.model = body.value("model", std::string{});
            if (reply.model != config_.model)
                return std::unexpected(agent::DecisionError{agent::DecisionError::Kind::invalid_response,
                                                            "decision response model does not match the configuration"});
            const auto answers = body.find("answers");
            if (answers == body.end() || !answers->is_object())
                return std::unexpected(agent::DecisionError{agent::DecisionError::Kind::invalid_response,
                                                            "decision response has no answers object"});
            for (const agent::DecisionQuestion& question : request.questions) {
                const std::string key = dimension_name(question.dimension);
                const auto found = answers->find(key);
                if (found == answers->end() || !found->is_object())
                    return std::unexpected(agent::DecisionError{agent::DecisionError::Kind::invalid_response,
                                                                std::format("decision response has no answers.{}", key)});
                if (question.criteria.empty()) {
                    const auto score = found->find("noul");
                    if (score == found->end() || !score->is_number())
                        return std::unexpected(agent::DecisionError{
                            agent::DecisionError::Kind::invalid_response,
                            std::format("answers.{}.noul must be a number", key)});
                    const double value = score->get<double>();
                    if (!std::isfinite(value) || value < 0.0 || value > 1.0)
                        return std::unexpected(agent::DecisionError{agent::DecisionError::Kind::invalid_response,
                                                                    "decision probability is outside [0, 1]"});
                    reply.scores.emplace(question.dimension, value);
                    continue;
                }
                agent::DirectionScore score;
                const auto selected = found->find("choice");
                if (selected == found->end() || !selected->is_string())
                    return std::unexpected(agent::DecisionError{
                        agent::DecisionError::Kind::invalid_response,
                        std::format("answers.{}.choice must be a string", key)});
                const auto direction = direction_from(selected->get<std::string>());
                if (!direction)
                    return std::unexpected(agent::DecisionError{
                        agent::DecisionError::Kind::invalid_response,
                        std::format("answers.{}.choice has an unknown category", key)});
                score.selected = *direction;
                const auto probabilities = found->find("probabilities");
                if (probabilities == found->end() || !probabilities->is_object())
                    return std::unexpected(agent::DecisionError{
                        agent::DecisionError::Kind::invalid_response,
                        std::format("answers.{}.probabilities must be an object", key)});
                for (const auto& [category, description] : question.criteria) {
                    const auto value = probabilities->find(direction_name(category));
                    if (value == probabilities->end() || !value->is_number())
                        return std::unexpected(agent::DecisionError{agent::DecisionError::Kind::invalid_response,
                                                                    "decision response is missing a category probability"});
                    const double probability = value->get<double>();
                    if (!std::isfinite(probability) || probability < 0.0 || probability > 1.0)
                        return std::unexpected(agent::DecisionError{agent::DecisionError::Kind::invalid_response,
                                                                    "decision probability is outside [0, 1]"});
                    score.probabilities.emplace(category, probability);
                }
                reply.scores.emplace(question.dimension, std::move(score));
            }
            if (const auto usage = body.find("usage"); usage != body.end() && usage->is_object()) {
                reply.usage.prompt = usage->value("input_tokens", std::int64_t{0});
                reply.usage.completion = usage->value("output_tokens", std::int64_t{0});
            }
            return reply;
        } catch (const json::exception& error) {
            return std::unexpected(agent::DecisionError{agent::DecisionError::Kind::invalid_response,
                                                        error.what()});
        }
    }

private:
    json encode(const agent::DecisionRequest& request) const {
        json questions = json::object();
        for (const agent::DecisionQuestion& question : request.questions) {
            json item{{"instructions", question.instructions}};
            if (question.criteria.empty()) {
                item["type"] = "noul";
            } else {
                item["type"] = "choice";
                json criteria = json::object();
                for (const auto& [direction, description] : question.criteria)
                    criteria[direction_name(direction)] = description;
                item["criteria"] = std::move(criteria);
            }
            questions[dimension_name(question.dimension)] = std::move(item);
        }
        return json{{"state", request.state}, {"questions", std::move(questions)}};
    }

    DecisionProviderConfig config_;
    std::chrono::milliseconds timeout_;
    net::HttpClient client_;
    std::mutex mutex_;
    std::condition_variable_any available_;
    bool busy_ = false; ///< 仅串行使用 HttpClient；排队等待可被 stop/deadline 唤醒。
};

} // namespace

std::shared_ptr<agent::DecisionModel> make_systemone_decision_model(
    DecisionProviderConfig config, std::chrono::milliseconds timeout, net::HttpOptions http) {
    log_llm()->info("decision model: kind=systemone endpoint={} model={} revision={}", config.endpoint,
                    config.model, config.model_revision);
    return std::make_shared<SystemOneDecisionModel>(std::move(config), timeout, std::move(http));
}

std::vector<DecisionProviderInfo> decision_providers() {
    return {{"systemone", kAdapterVersion, DecisionCapabilities{true, true}}};
}

} // namespace dagent::llm
