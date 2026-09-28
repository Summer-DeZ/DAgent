#include "backend/backend.hpp"

#include <chrono>
#include <format>
#include <stdexcept>
#include <utility>
#include <unistd.h>

#include <variant>

#include "backend/convert.hpp"
#include "base/log.hpp"
#include "base/text.hpp"

namespace dagent::backend {
namespace {

std::shared_ptr<spdlog::logger> log_backend() { return base::logger("backend"); }

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

// 末尾不完整的 UTF-8 序列从哪里开始；单个 bash 调用的块之间保留边界。
std::size_t complete_prefix(std::string_view text) {
    std::size_t index = text.size();
    std::size_t continuation = 0;
    while (index > 0 && continuation < 3 && (static_cast<unsigned char>(text[index - 1]) & 0xC0) == 0x80) {
        --index;
        ++continuation;
    }
    if (index == 0) return text.size();
    const auto lead = static_cast<unsigned char>(text[index - 1]);
    const std::size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return continuation + 1 < need ? index - 1 : text.size();
}

std::int64_t epoch_ms(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

protocol::RpcError invalid_params(std::string message) {
    protocol::RpcError error;
    error.code = protocol::rpc_code::kInvalidParams;
    error.message = std::move(message);
    return error;
}

std::string require_string(const nlohmann::json& params, const char* key, protocol::RpcError& error) {
    const auto it = params.find(key);
    if (it == params.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
        error = invalid_params(std::format("{} is required", key));
        return {};
    }
    return it->get<std::string>();
}

protocol::RpcError query_error(const runtime::QueryError& error) {
    protocol::RpcError out;
    out.code = protocol::rpc_code::kBusinessError;
    out.message = error.what();
    switch (error.kind()) {
    case runtime::QueryError::Kind::invalid_state:
        out.kind = protocol::error_kind::kInvalidState;
        break;
    case runtime::QueryError::Kind::not_found:
        out.kind = protocol::error_kind::kNotFound;
        break;
    case runtime::QueryError::Kind::query_failed:
        out.kind = protocol::error_kind::kQueryFailed;
        break;
    }
    return out;
}

nlohmann::json approval_payload(const agent::Approval& approval) {
    std::string preview_kind = "text";
    std::string preview = approval.intent.preview;
    switch (approval.intent.kind) {
    case agent::ToolKind::write: preview_kind = "diff"; break;
    case agent::ToolKind::exec:
        preview_kind = "code";
        if (approval.intent.command) preview = approval.intent.command->command;
        break;
    case agent::ToolKind::read:
        for (const auto& path : approval.intent.paths) preview += path.path.string() + '\n';
        break;
    case agent::ToolKind::external: preview_kind = "code"; break;
    }
    nlohmann::json requests = nlohmann::json::array();
    for (const agent::Approval::Request& request : approval.requests) {
        std::string kind;
        switch (request.kind) {
        case agent::Approval::Request::Kind::dynamic_command: kind = "dynamic_command"; break;
        case agent::Approval::Request::Kind::read_path: kind = "read_path"; break;
        case agent::Approval::Request::Kind::write_path: kind = "write_path"; break;
        case agent::Approval::Request::Kind::network: kind = "network"; break;
        case agent::Approval::Request::Kind::sensitive_read: kind = "sensitive_read"; break;
        case agent::Approval::Request::Kind::protected_write: kind = "protected_write"; break;
        case agent::Approval::Request::Kind::host_access: kind = "host_access"; break;
        }
        requests.push_back({{"kind", kind}, {"target", request.target}, {"reason", request.reason}});
    }
    return nlohmann::json{
        {"tool", approval.tool},
        {"agent", approval.agent},
        {"reason", approval.reason},
        {"summary", approval.intent.summary},
        {"preview_kind", preview_kind},
        {"preview_text", preview},
        {"cwd", approval.cwd},
        {"mode", approval.mode},
        {"requests", std::move(requests)},
        {"session_rule", approval.session_rule},
        {"can_network", approval.can_network},
        {"partially_executed", approval.partially_executed},
    };
}

nlohmann::json question_payload(const agent::Question& question) {
    nlohmann::json options = nlohmann::json::array();
    for (const agent::Question::Option& option : question.options) {
        options.push_back({{"label", option.label}, {"description", option.description}});
    }
    return nlohmann::json{{"header", question.header},
                          {"prompt", question.prompt},
                          {"options", std::move(options)},
                          {"multi_select", question.multi_select},
                          {"allow_other", question.allow_other}};
}

} // namespace

Backend::Backend(ipc::Channel channel, runtime::Assembler assemble)
    : channel_(std::move(channel)), assemble_(std::move(assemble)) {
    publisher_ = std::make_unique<Publisher>(channel_, [this] {
        closing_ = true;
        channel_.shutdown();
    });
}

Backend::~Backend() { quit(); }

int Backend::run() {
    publisher_->start();
    command_thread_ = std::jthread([this](std::stop_token stop) {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(command_mutex_);
                command_cv_.wait(lock, stop, [&] { return command_closed_ || !command_jobs_.empty(); });
                if (command_jobs_.empty()) {
                    if (command_closed_ || stop.stop_requested()) return;
                    continue;
                }
                job = std::move(command_jobs_.front());
                command_jobs_.pop_front();
            }
            handle_command(std::move(job));
        }
    });
    query_thread_ = std::jthread([this](std::stop_token stop) {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(query_mutex_);
                query_cv_.wait(lock, stop, [&] { return query_closed_ || !query_jobs_.empty(); });
                if (query_jobs_.empty()) {
                    if (query_closed_ || stop.stop_requested()) return;
                    continue;
                }
                job = std::move(query_jobs_.front());
                query_jobs_.pop_front();
            }
            handle_query(std::move(job));
        }
    });

    while (!closing_) {
        std::optional<std::string> line = channel_.receive_line();
        if (!line) break;
        protocol::Message message = protocol::parse_message(*line);
        if (message.kind == protocol::Message::Kind::request) {
            dispatch(message.request);
        } else if (message.kind == protocol::Message::Kind::notification) {
            continue; // 前端不发送业务通知
        } else if (message.kind == protocol::Message::Kind::invalid) {
            protocol::RpcError error;
            error.code = protocol::rpc_code::kParseError;
            error.message = message.error;
            publisher_->send_control_line(protocol::encode_error("", error));
        }
    }
    quit();
    channel_.close();
    return 0;
}

void Backend::quit() {
    closing_ = true;
    {
        const std::lock_guard lock(command_mutex_);
        command_closed_ = true;
        command_jobs_.clear();
    }
    command_cv_.notify_all();
    command_thread_.request_stop();
    if (command_thread_.joinable()) command_thread_.join();
    {
        const std::lock_guard lock(query_mutex_);
        query_closed_ = true;
        query_jobs_.clear();
    }
    query_cv_.notify_all();
    query_thread_.request_stop();
    if (query_thread_.joinable()) query_thread_.join();
    histories_.clear();
    if (runtime_) runtime_->shutdown();
    if (publisher_) {
        publisher_->flush();
        publisher_->stop();
    }
}

void Backend::respond(const std::string& id, nlohmann::json result) {
    publisher_->send_control_line(protocol::encode_result(id, std::move(result)));
}

void Backend::respond_snapshot(const std::string& id) {
    publisher_->send_snapshot(id, [this] { return runtime_->snapshot(); });
}

void Backend::fail(const std::string& id, const protocol::RpcError& error) {
    publisher_->send_control_line(protocol::encode_error(id, error));
}

void Backend::fail_target(const std::string& id, const runtime::RuntimeError& error) {
    fail(id, to_rpc_error(error));
}

void Backend::push_command(Job job) {
    {
        const std::lock_guard lock(command_mutex_);
        if (command_closed_) return;
        command_jobs_.push_back(std::move(job));
    }
    command_cv_.notify_all();
}

void Backend::push_query(Job job) {
    {
        const std::lock_guard lock(query_mutex_);
        if (query_closed_) return;
        query_jobs_.push_back(std::move(job));
    }
    query_cv_.notify_all();
}

void Backend::dispatch(const protocol::Request& request) {
    const std::string& id = request.id;
    const std::string& method = request.method;
    const nlohmann::json& params = request.params;

    if (method == "backend.hello") {
        if (!params.contains("protocol_version") || !params["protocol_version"].is_object()) {
            fail(id, invalid_params("protocol_version is required"));
            return;
        }
        const int major = params["protocol_version"].value("major", 0);
        const int minor = params["protocol_version"].value("minor", 0);
        if (major != protocol::kMajor || minor != protocol::kMinor) {
            protocol::RpcError mismatch;
            mismatch.code = protocol::rpc_code::kBusinessError;
            mismatch.kind = protocol::error_kind::kVersionMismatch;
            mismatch.message = std::format("backend speaks {}.{}, frontend sent {}.{}", protocol::kMajor,
                                           protocol::kMinor, major, minor);
            fail(id, mismatch);
            closing_ = true;
            return;
        }
        respond(id, {{"protocol_version", {{"major", protocol::kMajor}, {"minor", protocol::kMinor}}},
                     {"backend_version", DAGENT_VERSION},
                     {"backend_instance_id", std::format("{}-{}", ::getpid(), epoch_ms(std::chrono::system_clock::now()))}});
        return;
    }
    if (method == "app.initialize") {
        push_command(Job{id, method, params});
        return;
    }
    if (method == "backend.shutdown") {
        quit();
        // 收尾完成后直接写响应，随后关闭连接。
        channel_.send_line(protocol::encode_result(id, {{"closed", true}}));
        channel_.close();
        closing_ = true;
        return;
    }
    if (method == "interaction.answer") {
        const std::string interaction_id = params.value("interaction_id", "");
        if (!runtime_) {
            fail(id, to_rpc_error(runtime::RuntimeError{runtime::RuntimeError::Kind::invalid_state,
                                                        "the backend is not initialized"}));
            return;
        }
        bool accepted = false;
        const nlohmann::json answer = params.value("answer", nlohmann::json::object());
        if (answer.contains("selected") || answer.contains("other") || answer.contains("cancelled")) {
            agent::Answer parsed;
            parsed.selected = answer.value("selected", std::vector<int>{});
            parsed.other = answer.value("other", "");
            parsed.cancelled = answer.value("cancelled", false);
            accepted = runtime_->answer(interaction_id, std::move(parsed));
        } else {
            agent::Decision decision;
            const std::string value = answer.value("decision", "deny");
            decision.answer = value == "allow" ? agent::Decision::Answer::allow
                              : value == "allow_session" ? agent::Decision::Answer::allow_session
                              : value == "deny_with_feedback" ? agent::Decision::Answer::deny_with_feedback
                                                              : agent::Decision::Answer::deny;
            decision.feedback = answer.value("feedback", "");
            decision.network = answer.value("network", false);
            accepted = runtime_->answer(interaction_id, std::move(decision));
        }
        if (!accepted) {
            protocol::RpcError error;
            error.code = protocol::rpc_code::kBusinessError;
            error.kind = protocol::error_kind::kInteractionClosed;
            error.message = "the interaction is already closed";
            fail(id, error);
            return;
        }
        respond(id, {{"interaction_id", interaction_id}, {"accepted", true}});
        return;
    }
    if (method == "run.cancel") {
        if (!check_target(id, params)) return;
        const std::string run_id = params.value("run_id", "");
        const bool cancelled = runtime_->cancel(run_id);
        respond(id, {{"run_id", run_id},
                     {"result", cancelled ? "cancel_requested" : "already_finished"}});
        return;
    }
    if (method == "session.snapshot") {
        if (!check_target(id, params)) return;
        respond_snapshot(id);
        return;
    }
    if (method == "session.grants") {
        if (!check_target(id, params)) return;
        nlohmann::json grants = nlohmann::json::array();
        for (const agent::Policy::SessionGrant& grant : runtime_->snapshot().grants) {
            grants.push_back({{"id", grant.id}, {"description", grant.description}});
        }
        respond(id, {{"grants", std::move(grants)}});
        return;
    }
    if (method == "session.revoke_grant") {
        if (!check_target(id, params)) return;
        const std::string grant_id = params.value("grant_id", "");
        const auto accepted = runtime_->revoke_grant(
            grant_id, [this, id, grant_id](std::expected<bool, runtime::RuntimeError> removed) {
                if (!removed) fail_target(id, removed.error());
                else respond(id, {{"grant_id", grant_id}, {"removed", *removed}});
            });
        if (!accepted) fail_target(id, accepted.error());
        return;
    }
    if (method == "session.cycle_permission") {
        if (!check_target(id, params)) return;
        const auto result = runtime_->cycle_permission();
        if (!result) {
            fail_target(id, result.error());
            return;
        }
        respond_snapshot(id);
        return;
    }
    if (method == "session.toggle_planning") {
        if (!check_target(id, params)) return;
        const auto result = runtime_->toggle_planning();
        if (!result) {
            fail_target(id, result.error());
            return;
        }
        respond_snapshot(id);
        return;
    }
    if (method == "model.list") {
        if (!configuration_) {
            fail(id, to_rpc_error(runtime::RuntimeError{runtime::RuntimeError::Kind::invalid_state,
                                                        "the backend is not initialized"}));
            return;
        }
        nlohmann::json models = nlohmann::json::array();
        for (const agent::PublicModel& model : configuration_->models()) {
            models.push_back(to_protocol(model));
        }
        nlohmann::json kinds = nlohmann::json::array();
        for (const agent::ProviderKindInfo& kind : configuration_->provider_kinds()) {
            protocol::ProviderKind dto;
            dto.kind = kind.kind;
            dto.default_base_url = kind.default_base_url;
            dto.needs_credential = kind.needs_credential;
            kinds.push_back(std::move(dto));
        }
        nlohmann::json selected = nullptr;
        if (runtime_) {
            const runtime::RuntimeSnapshot snapshot = runtime_->snapshot();
            if (!snapshot.session_id.empty()) selected = snapshot.model.name;
        }
        respond(id, {{"models", std::move(models)},
                     {"provider_kinds", std::move(kinds)},
                     {"default_name", default_model_},
                     {"selected_name", std::move(selected)}});
        return;
    }
    if (method == "session.list" || method == "session.children" || method == "session.history" ||
        method == "session.history_close" || method == "workspace.info" ||
        method == "workspace.complete" || method == "skills.list") {
        push_query(Job{id, method, params});
        return;
    }
    if (method == "input.submit") {
        if (!check_target(id, params)) return;
        const std::string text = params.value("text", "");
        // 接受响应在 accepted 回调里先入发送队列；SessionController 在此之前不允许该输入出队，
        // 保证对应 TurnStarted 不会先于本响应（协议 §4.2）。
        const auto accepted = runtime_->submit(text, [this, id](const std::string& input_id) {
            respond(id, {{"input_id", input_id}, {"accepted", true}});
        });
        if (!accepted) fail_target(id, accepted.error());
        return;
    }
    if (method == "input.recall_last") {
        if (!check_target(id, params)) return;
        const auto recalled = runtime_->recall_last();
        nlohmann::json input = nullptr;
        if (recalled) input = nlohmann::json{{"input_id", recalled->id}, {"text", recalled->text}};
        respond(id, {{"input", std::move(input)}});
        return;
    }
    if (method == "session.new" || method == "session.resume" || method == "session.select_model" ||
        method == "model.add" || method == "session.compact") {
        push_command(Job{id, method, params});
        return;
    }
    protocol::RpcError error;
    error.code = protocol::rpc_code::kMethodNotFound;
    error.message = "unknown method: " + method;
    fail(id, error);
}

bool Backend::check_target(const std::string& id, const nlohmann::json& params) {
    if (!runtime_) {
        fail(id, to_rpc_error(runtime::RuntimeError{runtime::RuntimeError::Kind::invalid_state,
                                                    "the backend is not initialized"}));
        return false;
    }
    const runtime::RuntimeSnapshot snapshot = runtime_->snapshot();
    if (snapshot.session_id.empty()) {
        fail(id, to_rpc_error(runtime::RuntimeError{runtime::RuntimeError::Kind::invalid_state,
                                                    "there is no current session"}));
        return false;
    }
    const std::string session_id = params.value("session_id", "");
    const std::uint64_t generation = params.value("session_generation", std::uint64_t{0});
    if (session_id != snapshot.session_id || generation != snapshot.generation) {
        protocol::RpcError error;
        error.code = protocol::rpc_code::kBusinessError;
        error.kind = protocol::error_kind::kStaleSession;
        error.message = "the operation targets a stale session";
        fail(id, error);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 命令线程

void Backend::handle_command(Job job) {
    try {
        if (job.method == "app.initialize") {
            initialize(job.id, job.params);
        } else if (!initialized_) {
            fail(job.id, to_rpc_error(runtime::RuntimeError{runtime::RuntimeError::Kind::startup_failed,
                                                            "app.initialize has not completed"}));
        } else if (job.method == "session.new") {
            new_session(job.id, job.params);
        } else if (job.method == "session.resume") {
            resume_session(job.id, job.params);
        } else if (job.method == "session.select_model") {
            select_model(job.id, job.params);
        } else if (job.method == "model.add") {
            add_model(job.id, job.params);
        } else if (job.method == "session.compact") {
            compact(job.id, job.params);
        }
    } catch (const std::exception& error) {
        log_backend()->error("command {} failed: {}", job.method, error.what());
        protocol::RpcError rpc;
        rpc.code = protocol::rpc_code::kInternalError;
        rpc.message = error.what();
        fail(job.id, rpc);
    }
}

void Backend::initialize(const std::string& id, const nlohmann::json& params) {
    if (initialized_) {
        protocol::RpcError error;
        error.code = protocol::rpc_code::kBusinessError;
        error.kind = protocol::error_kind::kInvalidState;
        error.message = "the backend is already initialized";
        fail(id, error);
        return;
    }
    protocol::RpcError error;
    const std::string root = require_string(params, "root", error);
    if (!error.message.empty()) {
        fail(id, error);
        return;
    }
    const std::string cwd = require_string(params, "cwd", error);
    if (!error.message.empty()) {
        fail(id, error);
        return;
    }
    mode_ = params.value("mode", "interactive");
    runtime::BootstrapOptions options;
    options.mode = mode_;
    options.root = root;
    options.cwd = cwd;
    options.overrides = params.value("ordered_overrides", std::vector<std::string>{});
    if (params.contains("permissions") && params["permissions"].is_string()) {
        options.permissions = params["permissions"].get<std::string>();
    }
    options.read_only = params.value("read_only", false);
    options.plan = params.value("plan", false);
    if (params.contains("log_level") && params["log_level"].is_string()) {
        options.log_level = params["log_level"].get<std::string>();
    }

    runtime::Assembled assembled;
    try {
        assembled = assemble_(options);
    } catch (const runtime::ConfigurationError& config_error) {
        protocol::RpcError rpc;
        rpc.code = protocol::rpc_code::kBusinessError;
        rpc.kind = protocol::error_kind::kConfigError;
        rpc.message = config_error.what();
        fail(id, rpc);
        return;
    }
    if (!assembled.maintenance.is_null()) {
        initialized_ = true;
        respond(id, {{"maintenance", std::move(assembled.maintenance)}});
        return;
    }
    default_model_ = assembled.default_model;
    progress_interval_ms_ = assembled.progress_interval_ms;
    queries_ = assembled.queries;

    const bool query_only = mode_ == "sessions" || mode_ == "models";
    bool resumed = false;
    nlohmann::json session_json = nullptr;
    if (!query_only) {
        runtime::Runtime::Deps deps;
        deps.configuration = assembled.configuration;
        deps.factory = std::move(assembled.factory);
        deps.frontend = this;
        deps.interactive = mode_ == "interactive";
        runtime_ = std::make_unique<runtime::Runtime>(std::move(deps));
        runtime::StartOptions start;
        if (params.contains("resume_id") && params["resume_id"].is_string()) {
            start.resume_id = params["resume_id"].get<std::string>();
        }
        start.continue_last = params.value("continue_last", false);
        try {
            resumed = runtime_->start(start);
        } catch (const std::exception& error) {
            runtime_.reset();
            protocol::RpcError rpc;
            rpc.code = protocol::rpc_code::kBusinessError;
            rpc.kind = protocol::error_kind::kStartupFailed;
            rpc.message = error.what();
            fail(id, rpc);
            return;
        }
        session_json = nlohmann::json(to_protocol(runtime_->snapshot()));
    }
    configuration_ = assembled.configuration;
    initialized_ = true;

    nlohmann::json theme = nullptr;
    if (const auto file = configuration_->theme_file(); !file.empty()) theme = file.string();
    respond(id, {{"mode", mode_},
                 {"session", std::move(session_json)},
                 {"resumed", resumed},
                 {"ui", {{"theme_file", std::move(theme)}}},
                 {"progress_interval_ms", progress_interval_ms_}});
}

void Backend::new_session(const std::string& id, const nlohmann::json& params) {
    if (!check_target(id, params)) return;
    const auto accepted = runtime_->new_session([this, id](std::expected<void, runtime::RuntimeError> result) {
        if (!result) {
            fail_target(id, result.error());
            return;
        }
        respond_snapshot(id);
    });
    if (!accepted) fail_target(id, accepted.error());
}

void Backend::resume_session(const std::string& id, const nlohmann::json& params) {
    std::string target = params.value("id", "");
    const bool continue_last = params.value("continue_last", false);
    const runtime::RuntimeSnapshot snapshot = runtime_->snapshot();
    if (!snapshot.session_id.empty() && !check_target(id, params)) return;
    if (target.empty() && !continue_last) {
        fail(id, invalid_params("id or continue_last is required"));
        return;
    }
    std::optional<std::string_view> prefix;
    if (!target.empty()) prefix = target;
    const auto resolved = runtime_->resolve_session(prefix);
    if (!resolved) {
        fail_target(id, resolved.error());
        return;
    }
    const auto accepted =
        runtime_->resume(*resolved, [this, id](std::expected<void, runtime::RuntimeError> result) {
            if (!result) {
                fail_target(id, result.error());
                return;
            }
            respond_snapshot(id);
        });
    if (!accepted) fail_target(id, accepted.error());
}

void Backend::select_model(const std::string& id, const nlohmann::json& params) {
    if (!check_target(id, params)) return;
    protocol::RpcError error;
    const std::string name = require_string(params, "name", error);
    if (!error.message.empty()) {
        fail(id, error);
        return;
    }
    const auto accepted =
        runtime_->select_model(name, [this, id](std::expected<void, runtime::RuntimeError> result) {
            if (!result) {
                fail_target(id, result.error());
                return;
            }
            respond_snapshot(id);
        });
    if (!accepted) fail_target(id, accepted.error());
}

void Backend::add_model(const std::string& id, const nlohmann::json& params) {
    if (!check_target(id, params)) return;
    agent::ModelInput input;
    input.kind = params.value("kind", "");
    input.name = params.value("name", "");
    input.base_url = params.value("base_url", "");
    input.model = params.value("model", "");
    input.credential = params.value("credential", "");
    input.max_tokens = params.value("max_tokens", std::size_t{8192});
    input.context_window = params.value("context_window", std::size_t{0});
    if (input.kind.empty() || input.name.empty() || input.model.empty()) {
        fail(id, invalid_params("kind, name and model are required"));
        return;
    }
    const auto accepted = runtime_->add_model(
        std::move(input), [this, id](agent::PublicModel saved, std::string selection_error) {
            if (saved.name.empty()) {
                protocol::RpcError rpc;
                rpc.code = protocol::rpc_code::kBusinessError;
                rpc.kind = protocol::error_kind::kConfigError;
                rpc.message = selection_error.empty() ? "failed to save the model" : selection_error;
                fail(id, rpc);
                return;
            }
            respond(id, {{"model", to_protocol(saved)},
                         {"selected", selection_error.empty()},
                         {"selection_error", selection_error.empty() ? nlohmann::json(nullptr)
                                                                     : nlohmann::json(selection_error)}});
        });
    if (!accepted) fail_target(id, accepted.error());
}

void Backend::compact(const std::string& id, const nlohmann::json& params) {
    if (!check_target(id, params)) return;
    const auto accepted = runtime_->compact();
    if (!accepted) {
        fail_target(id, accepted.error());
        return;
    }
    std::string operation_id;
    {
        const std::lock_guard lock(state_mutex_);
        operation_id = std::format("op-{}", ++compact_seq_);
        pending_compact_id_ = operation_id;
    }
    respond(id, {{"operation_id", operation_id}, {"accepted", true}});
}

// ---------------------------------------------------------------- 查询线程

void Backend::handle_query(Job job) {
    try {
        if (job.method == "session.list") {
            list_sessions(job.id, job.params);
        } else if (job.method == "session.children") {
            list_children(job.id, job.params);
        } else if (job.method == "session.history") {
            history(job.id, job.params);
        } else if (job.method == "session.history_close") {
            history_close(job.id, job.params);
        } else if (job.method == "workspace.info") {
            workspace_info(job.id, job.params);
        } else if (job.method == "skills.list") {
            const auto& catalog = queries_->skills();
            nlohmann::json skills = nlohmann::json::array(), diagnostics = nlohmann::json::array();
            for (const auto& skill : catalog.definitions)
                skills.push_back({{"name", skill.name}, {"description", skill.description},
                                  {"path", skill.file.string()}});
            for (const auto& diagnostic : catalog.diagnostics)
                diagnostics.push_back({{"path", diagnostic.path}, {"message", diagnostic.message}});
            respond(job.id, {{"skills", std::move(skills)}, {"diagnostics", std::move(diagnostics)}});
        } else if (job.method == "workspace.complete") {
            workspace_complete(job.id, job.params);
        }
    } catch (const runtime::QueryError& error) {
        fail(job.id, query_error(error));
    } catch (const std::exception& error) {
        protocol::RpcError rpc;
        rpc.code = protocol::rpc_code::kBusinessError;
        rpc.kind = protocol::error_kind::kQueryFailed;
        rpc.message = error.what();
        fail(job.id, rpc);
    }
}

void Backend::list_sessions(const std::string& id, const nlohmann::json& params) {
    if (!queries_) {
        fail(id, to_rpc_error(runtime::RuntimeError{runtime::RuntimeError::Kind::invalid_state,
                                                    "the backend is not initialized"}));
        return;
    }
    const std::size_t limit = params.value("limit", std::size_t{50});
    nlohmann::json sessions = nlohmann::json::array();
    for (const runtime::SessionSummary& summary : queries_->sessions(limit)) {
        sessions.push_back(
            {{"id", summary.id}, {"title", summary.title}, {"updated", epoch_ms(summary.updated)}});
    }
    respond(id, {{"sessions", std::move(sessions)}, {"next_cursor", nullptr}});
}

void Backend::list_children(const std::string& id, const nlohmann::json& params) {
    protocol::RpcError error;
    const std::string session_id = require_string(params, "session_id", error);
    if (!error.message.empty()) {
        fail(id, error);
        return;
    }
    nlohmann::json sessions = nlohmann::json::array();
    for (const runtime::ChildSummary& child : queries_->children(session_id)) {
        sessions.push_back(
            {{"session_id", child.session_id}, {"agent", child.agent}, {"title", child.title}});
    }
    respond(id, {{"sessions", std::move(sessions)}, {"next_cursor", nullptr}});
}

void Backend::history(const std::string& id, const nlohmann::json& params) {
    protocol::RpcError error;
    const std::string session_id = require_string(params, "session_id", error);
    if (!error.message.empty()) {
        fail(id, error);
        return;
    }
    const std::string cursor = params.value("cursor", "");
    const std::size_t limit = params.value("limit", std::size_t{100});
    std::unique_ptr<runtime::HistoryReader> reader;
    if (!cursor.empty()) {
        const auto it = histories_.find(cursor);
        if (it == histories_.end() || it->second->session_id() != session_id) {
            protocol::RpcError rpc;
            rpc.code = protocol::rpc_code::kBusinessError;
            rpc.kind = protocol::error_kind::kInvalidState;
            rpc.message = "the history cursor does not belong to this query";
            fail(id, rpc);
            return;
        }
        reader = std::move(it->second);
        histories_.erase(it);
    } else {
        reader = queries_->open_history(session_id);
    }
    runtime::HistoryPage page = reader->read(limit);
    const auto upper_seq = reader->upper_seq();
    nlohmann::json items = nlohmann::json::array();
    for (const agent::HistoryItem& item : page.items) items.push_back(to_protocol(item));
    nlohmann::json next = nullptr;
    if (!page.done) {
        const std::string token = std::format("history-{}", ++history_seq_);
        next = token;
        histories_.emplace(token, std::move(reader));
    }
    respond(id, {{"session_id", session_id},
                 {"upper_seq", upper_seq},
                 {"items", std::move(items)},
                 {"next_cursor", std::move(next)}});
}

void Backend::history_close(const std::string& id, const nlohmann::json& params) {
    const std::string cursor = params.value("cursor", "");
    if (!cursor.empty()) histories_.erase(cursor);
    respond(id, {{"closed", true}});
}

void Backend::workspace_info(const std::string& id, const nlohmann::json&) {
    const runtime::WorkspaceInfo info = queries_->workspace();
    respond(id, {{"cwd", info.cwd.string()},
                 {"project_root", info.project_root.string()},
                 {"branch", info.branch}});
}

void Backend::workspace_complete(const std::string& id, const nlohmann::json& params) {
    const std::string text = params.value("text", "");
    const std::size_t limit = params.value("limit", std::size_t{8});
    const std::vector<runtime::FileCandidate> candidates = queries_->complete(text, limit);
    nlohmann::json array = nlohmann::json::array();
    for (const runtime::FileCandidate& candidate : candidates) {
        array.push_back({{"path", candidate.path}, {"directory", candidate.directory}});
    }
    respond(id, {{"candidates", std::move(array)}, {"truncated", candidates.size() == limit}});
}

// ---------------------------------------------------------------- 事件

void Backend::event(const runtime::Event& event) {
    if (const auto* control = std::get_if<runtime::ControlEvent>(&event.payload)) {
        handle_control(*control, event);
        return;
    }
    handle_core(event);
}

void Backend::handle_control(const runtime::ControlEvent& control, const runtime::Event& event) {
    switch (control.kind) {
    case runtime::ControlEvent::Kind::session_replaced: {
        // 事件发出后客户端用 session.snapshot 取权威状态；这里不回调 Runtime，
        // 避免与控制器发布锁形成反向锁序。
        protocol::Event out;
        out.session_id = event.session_id;
        out.session_generation = event.generation;
        out.kind = "session.changed";
        out.data = {{"replace_transcript", control.replace_transcript}, {"resumed", control.resumed}};
        publisher_->send_event(std::move(out));
        break;
    }
    case runtime::ControlEvent::Kind::operation_finished: {
        std::string operation_id;
        {
            const std::lock_guard lock(state_mutex_);
            operation_id = pending_compact_id_;
            pending_compact_id_.clear();
        }
        protocol::Event out;
        out.session_id = event.session_id;
        out.session_generation = event.generation;
        out.kind = "operation.finished";
        out.data = {{"operation_id", operation_id},
                    {"status", std::string(agent::to_string(control.status))},
                    {"error", control.error}};
        publisher_->send_event(std::move(out));
        break;
    }
    case runtime::ControlEvent::Kind::failed: {
        protocol::Event out;
        out.session_id = event.session_id;
        out.session_generation = event.generation;
        out.kind = "notice";
        out.data = {{"level", "error"},
                    {"text", control.operation.empty() ? control.error
                                                       : control.operation + " failed: " + control.error}};
        publisher_->send_event(std::move(out));
        break;
    }
    }
}

void Backend::handle_core(const runtime::Event& event) {
    protocol::Event out;
    out.session_id = event.session_id;
    out.session_generation = event.generation;

    const agent::Event* core = &std::get<agent::Event>(event.payload);
    const agent::Event* inner = core;
    if (const auto* sub = std::get_if<agent::SubEvent>(core)) {
        out.session_id = sub->session;
        out.parent_session_id = event.session_id;
        out.model_call_id = sub->call_id;
        out.agent = sub->agent;
        inner = &sub->event();
    }

    // tool_output 的 UTF-8 边界缓冲必须在 JSON 编码之前按原始字节完成（协议 §7）：
    // split_event 会先做 to_valid_utf8，跨块的多字节字符会被替换掉。
    if (const agent::ToolOutput* output = raw_tool_output(*core)) {
        std::string data;
        {
            const std::lock_guard lock(chunk_mutex_);
            data = std::move(pending_chunks_[output->id]);
        }
        data += output->chunk;
        const std::size_t cut = complete_prefix(data);
        {
            const std::lock_guard lock(chunk_mutex_);
            pending_chunks_[output->id] = data.substr(cut);
        }
        data.resize(cut);
        if (data.empty()) return;
        out.kind = "tool_output";
        out.data = {{"id", output->id}, {"chunk", base::to_valid_utf8(data)}};
    } else {
        const auto [kind, data] = split_event(*inner);
        out.kind = kind;
        out.data = data;
    }

    if (out.kind == "tool_finished") {
        const std::string call_id = out.data.value("id", "");
        std::string leftover;
        {
            const std::lock_guard lock(chunk_mutex_);
            const auto it = pending_chunks_.find(call_id);
            if (it != pending_chunks_.end()) {
                leftover = std::move(it->second);
                pending_chunks_.erase(it);
            }
        }
        if (!leftover.empty()) {
            protocol::Event chunk;
            chunk.session_id = out.session_id;
            chunk.session_generation = out.session_generation;
            chunk.kind = "tool_output";
            chunk.data = {{"id", call_id}, {"chunk", base::to_valid_utf8(leftover)}};
            publisher_->send_event(std::move(chunk));
        }
    }
    publisher_->send_event(std::move(out));
}

void Backend::interaction_requested(const runtime::InteractionRequest& request) {
    protocol::InteractionRequest out;
    out.interaction_id = request.id;
    out.kind = request.kind == runtime::InteractionRequest::Kind::approval ? "approval" : "question";
    out.session_id = request.session_id;
    out.session_generation = request.generation;
    out.payload = request.kind == runtime::InteractionRequest::Kind::approval
                      ? approval_payload(request.approval)
                      : question_payload(request.question);
    protocol::Event event;
    event.kind = "interaction.requested";
    event.session_id = request.session_id;
    event.session_generation = request.generation;
    event.data = nlohmann::json(out);
    publisher_->send_event(std::move(event));
}

void Backend::interaction_closed(const std::string& interaction_id) {
    protocol::Event event;
    event.kind = "interaction.closed";
    event.data = {{"interaction_id", interaction_id}};
    if (runtime_) {
        const runtime::RuntimeSnapshot snapshot = runtime_->snapshot();
        event.session_id = snapshot.session_id;
        event.session_generation = snapshot.generation;
    }
    publisher_->send_event(std::move(event));
}

} // namespace dagent::backend
