#include "app/output.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>
#include <utility>

namespace dagent::app {
namespace {

// 与 agent/conversation.hpp 的 texts::kInterrupted 相同：text/json 结果里的用户中断标记。
constexpr std::string_view kInterrupted = "\n\n[response interrupted by the user]";

const char* level_name(const std::string& level) {
    if (level == "warn") return "warn";
    if (level == "error") return "error";
    return "info";
}

bool core_event_kind(std::string_view kind) {
    // 旧 JSONL 只包含核心实时事件；协议新增的会话/操作/交互通知不进公开输出（B25）。
    static constexpr std::array kinds{
        std::string_view{"turn_started"},  std::string_view{"step_started"},
        std::string_view{"text"},          std::string_view{"reasoning"},
        std::string_view{"stream_reset"},  std::string_view{"tool_pending"},
        std::string_view{"tool_started"},  std::string_view{"tool_output"},
        std::string_view{"tool_finished"}, std::string_view{"retrying"},
        std::string_view{"compacted"},     std::string_view{"context"},
        std::string_view{"model_changed"}, std::string_view{"mode_changed"},
        std::string_view{"notice"},        std::string_view{"turn_ended"},
    };
    return std::ranges::find(kinds, kind) != kinds.end();
}

/// @brief 协议事件 → 旧实时事件 JSON；子事件恢复原 sub_event 包装。
nlohmann::json legacy_json(const protocol::Event& event) {
    nlohmann::json data = event.data;
    data["type"] = event.kind;
    if (!event.parent_session_id) return data;
    return nlohmann::json{{"type", "sub_event"},
                          {"session", event.session_id},
                          {"agent", event.agent.value_or("")},
                          {"parent_call", event.model_call_id.value_or("")},
                          {"event", std::move(data)}};
}

} // namespace

LegacyOutputCodec::LegacyOutputCodec(OutputFormat format, std::ostream& out, std::ostream& err)
    : format_(format), out_(out), err_(err) {}

bool LegacyOutputCodec::session_header(const std::string& session_id, bool resumed) {
    session_id_ = session_id;
    if (format_ != OutputFormat::jsonl) return true;
    return write_line(nlohmann::json{{"type", "session"}, {"id", session_id}, {"resumed", resumed}});
}

bool LegacyOutputCodec::write_line(nlohmann::json line) {
    if (output_failed_) return false;
    out_ << line.dump() << '\n';
    out_.flush();
    if (out_) return true;
    output_failed_ = true;
    return false;
}

void LegacyOutputCodec::write_stderr(const std::string& line) { err_ << line << '\n'; }

void LegacyOutputCodec::stream_event(const protocol::Event& event) {
    if (event.parent_session_id) return; // text/json 只报最终结果，子 Agent 进度留给界面
    const nlohmann::json& data = event.data;
    if (event.kind == "step_started") {
        step_text_.clear();
        step_had_call_ = false;
        waiting_ = true;
        step_begin_ = std::chrono::steady_clock::now();
    } else if (event.kind == "text") {
        step_text_ += data.value("text", "");
        waiting_ = false;
    } else if (event.kind == "reasoning") {
        waiting_ = false;
    } else if (event.kind == "stream_reset") {
        step_text_.clear();
        step_had_call_ = false;
        write_stderr("... discarding partial output from this step");
    } else if (event.kind == "tool_pending") {
        step_had_call_ = true;
        waiting_ = false;
    } else if (event.kind == "tool_started") {
        write_stderr("→ " + data.value("summary", ""));
    } else if (event.kind == "tool_finished") {
        const bool bad = data.value("is_error", false) || data.value("interrupted", false);
        write_stderr((bad ? "✗ " : "✓ ") + data.value("summary", ""));
    } else if (event.kind == "retrying") {
        write_stderr("retry " + std::to_string(data.value("attempt", 0)) + "/" +
                     std::to_string(data.value("max_attempts", 0)) + ": " +
                     data.value("reason", ""));
    } else if (event.kind == "compacted") {
        write_stderr("compacted: " + std::to_string(data.value("before", 0)) + " → " +
                     std::to_string(data.value("after", 0)));
    } else if (event.kind == "notice") {
        write_stderr("[" + std::string(level_name(data.value("level", "info"))) + "] " +
                     data.value("text", ""));
    } else if (event.kind == "turn_ended") {
        status_ = data.value("status", "done");
        error_ = data.value("error", "");
        steps_ = data.value("steps", 0);
        tool_calls_ = data.value("tool_calls", 0);
        const auto usage = data.find("usage");
        if (usage != data.end() && usage->is_object()) {
            usage_.prompt = usage->value("prompt", std::int64_t{0});
            usage_.completion = usage->value("completion", std::int64_t{0});
            usage_.cached = usage->value("cached", std::int64_t{0});
        }
        waiting_ = false;
        if (!error_.empty()) write_stderr("✗ " + error_);
    }
}

void LegacyOutputCodec::jsonl_event(const protocol::Event& event) {
    if (!core_event_kind(event.kind)) return;
    if (event.kind == "notice") {
        const std::string level = event.data.value("level", "info");
        if (level != "info") {
            write_stderr("[" + std::string(level_name(level)) + "] " +
                         event.data.value("text", ""));
        }
    }
    write_line(legacy_json(event));
}

bool LegacyOutputCodec::handle(const protocol::Event& event) {
    const std::lock_guard lock(mutex_);
    if (format_ == OutputFormat::jsonl) {
        jsonl_event(event);
        return !output_failed_;
    }
    stream_event(event);
    return true;
}

void LegacyOutputCodec::heartbeat() {
    const std::lock_guard lock(mutex_);
    if (format_ == OutputFormat::jsonl || !waiting_) return;
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - step_begin_)
            .count();
    if (seconds < 1) return;
    write_stderr("waiting for the model... " + std::to_string(seconds) + "s");
}

LegacyOutputCodec::Summary LegacyOutputCodec::finish(std::int64_t duration_ms) {
    const std::lock_guard lock(mutex_);
    Summary summary;
    summary.status = status_;
    summary.error = error_;
    summary.steps = steps_;
    summary.tool_calls = tool_calls_;
    summary.usage = usage_;
    summary.output_failed = output_failed_;
    summary.result = step_text_;
    if (status_ == "interrupted" && !step_had_call_ && !summary.result.empty()) {
        summary.result += kInterrupted;
    }
    if (format_ == OutputFormat::jsonl) return summary;

    if (format_ == OutputFormat::json) {
        const nlohmann::json object = {
            {"session_id", session_id_},
            {"status", summary.status},
            {"error", summary.error},
            {"result", summary.result},
            {"steps", summary.steps},
            {"tool_calls", summary.tool_calls},
            {"usage", {{"prompt", summary.usage.prompt},
                       {"completion", summary.usage.completion},
                       {"cached", summary.usage.cached}}},
            {"duration_ms", duration_ms},
        };
        out_ << object.dump() << '\n';
        out_.flush();
    } else if (!summary.result.empty()) {
        out_ << summary.result;
        if (summary.result.back() != '\n') out_ << '\n';
        out_.flush();
    }
    return summary;
}

} // namespace dagent::app
