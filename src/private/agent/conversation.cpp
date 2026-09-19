#include "agent/conversation.hpp"

#include <cassert>
#include <format>
#include <set>
#include <utility>

#include "agent/llm.hpp"

namespace dagent::agent {

std::size_t Conversation::compute_tokens(const Message& message) const {
    std::size_t total = estimate_tokens(message.content) + estimate_tokens(message.reasoning_content) +
                        estimate_tokens(message.tool_call_id);
    for (const ToolCall& call : message.tool_calls) {
        total += estimate_tokens(call.id) + estimate_tokens(call.name) + estimate_tokens(call.arguments);
    }
    return total;
}

std::int64_t Conversation::add_user(std::string text) {
    Entry entry;
    entry.message.role = Role::user;
    entry.message.content = std::move(text);
    entry.ordinal = next_ordinal_++;
    entry.tokens = compute_tokens(entry.message);
    entries_.push_back(std::move(entry));
    return entries_.back().ordinal;
}

std::int64_t Conversation::add_assistant(Message message) {
    message.role = Role::assistant;
    Entry entry;
    entry.ordinal = next_ordinal_++;
    entry.tokens = compute_tokens(message);
    entry.message = std::move(message);
    entries_.push_back(std::move(entry));
    return entries_.back().ordinal;
}

std::int64_t Conversation::add_tool_result(std::string_view call_id, std::string text, std::string summary) {
    const std::vector<ToolCall> open = open_calls();
    assert(!open.empty() && open.front().id == call_id); // 调度器的编程错误

    Entry entry;
    entry.message.role = Role::tool;
    entry.message.tool_call_id = std::string(call_id);
    entry.message.content = std::move(text);
    entry.summary = std::move(summary);
    entry.ordinal = next_ordinal_++;
    entry.tokens = compute_tokens(entry.message);
    entries_.push_back(std::move(entry));
    return entries_.back().ordinal;
}

std::vector<ToolCall> Conversation::open_calls() const {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->message.role != Role::assistant || it->message.tool_calls.empty()) continue;
        std::set<std::string> answered;
        for (auto next = it.base(); next != entries_.end(); ++next) {
            if (next->message.role == Role::tool) answered.insert(next->message.tool_call_id);
        }
        std::vector<ToolCall> open;
        for (const ToolCall& call : it->message.tool_calls) {
            if (!answered.contains(call.id)) open.push_back(call);
        }
        return open;
    }
    return {};
}

std::size_t Conversation::tokens() const {
    std::size_t total = 0;
    for (const Entry& entry : entries_) total += entry.tokens;
    return total;
}

Request Conversation::build(const std::string& system, const std::vector<ToolDef>& tools,
                            const ModelParams& params) const {
    assert(!validate().has_value());

    Request request;
    request.model = params.model;
    request.max_tokens = params.max_tokens;
    request.temperature = params.temperature;
    request.stream = true;
    request.tools = tools;
    request.messages.reserve(entries_.size() + 1);

    Message prompt;
    prompt.role = Role::system;
    prompt.content = system;
    request.messages.push_back(std::move(prompt));
    for (const Entry& entry : entries_) request.messages.push_back(entry.message);
    return request;
}

std::optional<std::string> Conversation::validate() const {
    if (entries_.empty()) return std::nullopt;
    if (entries_.front().message.role != Role::user) return "I4：第一条不是 user 消息";

    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& entry = entries_[i];
        if (entry.message.role == Role::tool) {
            return std::format("I3：第 {} 条 tool 消息没有对应的 assistant 调用", i);
        }
        if (entry.message.role != Role::assistant) continue;
        if (entry.message.content.empty() && entry.message.tool_calls.empty()) {
            return std::format("I2：第 {} 条 assistant 既没有内容也没有工具调用", i);
        }
        if (entry.message.tool_calls.empty()) continue;

        std::size_t next = i + 1;
        for (const ToolCall& call : entry.message.tool_calls) {
            if (next >= entries_.size() || entries_[next].message.role != Role::tool) {
                return std::format("I1：第 {} 条 assistant 的调用 {} 没有对应的 tool 消息", i,
                                   call.name);
            }
            if (entries_[next].message.tool_call_id != call.id) {
                return std::format("I1：第 {} 条 tool 消息的 id 与调用不一致", next);
            }
            ++next;
        }
        i = next - 1;
    }
    return std::nullopt;
}

std::vector<std::size_t> Conversation::safe_cuts() const {
    std::vector<std::size_t> cuts;
    for (std::size_t i = 1; i < entries_.size(); ++i) {
        const Role role = entries_[i].message.role;
        if (role == Role::user || role == Role::assistant) cuts.push_back(i);
    }
    return cuts;
}

void Conversation::prune(std::size_t tool_entry, std::string placeholder) {
    Entry& entry = entries_[tool_entry];
    entry.message.content = std::move(placeholder);
    entry.pruned = true;
    entry.tokens = compute_tokens(entry.message);
}

void Conversation::replace_prefix(std::size_t cut, std::string summary_message) {
    entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(cut));
    Entry entry;
    entry.message.role = Role::user;
    entry.message.content = std::move(summary_message);
    entry.ordinal = -1;
    entry.tokens = compute_tokens(entry.message);
    entries_.push_front(std::move(entry));
}

void Conversation::restore(Entry entry) { entries_.push_back(std::move(entry)); }

void Conversation::set_next_ordinal(std::int64_t next) { next_ordinal_ = next; }

} // namespace dagent::agent
