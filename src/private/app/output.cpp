#include "app/output.hpp"

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

} // namespace

RunOutput::RunOutput(OutputFormat format, std::ostream& out, std::ostream& err)
    : format_(format), out_(out), err_(err) {}

void RunOutput::set_session(std::string session_id) {
    session_id_ = std::move(session_id);
}

bool RunOutput::write_line(nlohmann::json line) {
    if (output_failed_) return false;
    out_ << line.dump() << '\n';
    out_.flush();
    if (out_) return true;
    output_failed_ = true;
    return false;
}

void RunOutput::write_stderr(const std::string& line) { err_ << line << '\n'; }

void RunOutput::accumulate(const protocol::Event& event) {
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
    } else if (event.kind == "tool_pending") {
        step_had_call_ = true;
        waiting_ = false;
    } else if (event.kind == "turn_ended") {
        status_ = data.value("status", "done");
        error_ = data.value("error", "");
        steps_ = data.value("steps", 0);
        tool_calls_ = data.value("tool_calls", 0);
        usage_ = data.value("usage", nlohmann::json::object()).get<protocol::Usage>();
        waiting_ = false;
    }
}

void RunOutput::progress(const protocol::Event& event) {
    const nlohmann::json& data = event.data;
    if (event.kind == "stream_reset") {
        write_stderr("... discarding partial output from this step");
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
    } else if (event.kind == "turn_ended" && !error_.empty()) {
        write_stderr("✗ " + error_);
    }
}

bool RunOutput::handle(const protocol::Event& event) {
    const std::lock_guard lock(mutex_);
    if (!event.parent_session_id) accumulate(event);
    if (format_ == OutputFormat::jsonl) {
        if (event.kind == "notice" && event.data.value("level", "info") != "info")
            progress(event);
        return write_line(nlohmann::json(event));
    }
    if (!event.parent_session_id) progress(event);
    return true;
}

void RunOutput::heartbeat() {
    const std::lock_guard lock(mutex_);
    if (format_ == OutputFormat::jsonl || !waiting_) return;
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - step_begin_)
            .count();
    if (seconds < 1) return;
    write_stderr("waiting for the model... " + std::to_string(seconds) + "s");
}

RunOutput::Summary RunOutput::finish(std::int64_t duration_ms) {
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
