#include "agent/compaction.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "base/log.hpp"

namespace dagent::agent {
namespace {

void check_stop(std::stop_token stop) {
    if (stop.stop_requested()) throw ModelError(ModelError::Kind::cancelled, {}, "compaction cancelled");
}

std::size_t protected_begin(const Conversation& conversation, std::size_t target) {
    const auto& entries = conversation.entries();
    std::size_t begin = entries.size(), tokens = 0;
    while (begin > 0) {
        tokens += entries[--begin].tokens;
        if (tokens > target / 4) break;
    }
    // 保护区不得拆开调用批；即使末尾还有自然语言回复，也保留最近一批工具。
    for (std::size_t i = entries.size(); i > 0; --i) {
        if (!entries[i - 1].message.tool_calls.empty()) {
            begin = std::min(begin, i - 1);
            break;
        }
    }
    while (begin > 0 && entries[begin].message.role == Role::tool) --begin;
    return begin;
}

std::size_t summary_cut(const Conversation& conversation, std::size_t protection,
                        std::size_t target, std::size_t minimum) {
    const auto& entries = conversation.entries();
    std::size_t tail = conversation.tokens(), last = 0, previous = 0;
    bool has_assistant = false;
    for (const std::size_t cut : conversation.safe_cuts()) {
        if (cut > protection) break;
        for (; previous < cut; ++previous) {
            tail -= entries[previous].tokens;
            has_assistant |= entries[previous].message.role == Role::assistant;
        }
        if (!has_assistant || cut < minimum) continue;
        last = cut;
        if (tail <= target / 2) return cut;
    }
    return last;
}

Request summary_request(const Conversation& conversation, std::size_t cut,
                         const ModelParams& params, const std::string& prompt,
                         TokenEstimator& estimator, std::size_t limit) {
    Request request;
    request.model = params.model;
    request.max_tokens = params.max_tokens;
    request.temperature = params.temperature;
    request.stream = true;
    Message system;
    system.role = Role::system;
    system.content = prompt;
    request.messages.push_back(std::move(system));
    for (std::size_t i = 0; i < cut; ++i) {
        request.messages.push_back(conversation.entries()[i].message);
    }
    Message instruction;
    instruction.role = Role::user;
    instruction.content = "Summarize the conversation above as instructed.";
    request.messages.push_back(std::move(instruction));

    // 先保留原文；只有摘要请求超预算时，才从最旧的工具输出开始换成占位。
    auto estimated = estimator.estimate(request);
    for (std::size_t i = 0; i < cut && estimated > limit; ++i) {
        const Entry& entry = conversation.entries()[i];
        if (entry.message.role != Role::tool || entry.pruned) continue;
        request.messages[i + 1].content = texts::pruned_output(entry.summary);
        estimated = estimator.estimate(request);
    }
    // 再丢完整的 assistant 批；用户请求和旧摘要排在最后，且始终保留总结指令。
    while (estimated > limit && request.messages.size() > 2) {
        auto begin = std::find_if(request.messages.begin() + 1, request.messages.end() - 1,
                                  [](const Message& m) { return m.role == Role::assistant; });
        if (begin == request.messages.end() - 1) begin = request.messages.begin() + 1;
        auto end = begin + 1;
        while (end != request.messages.end() - 1 && end->role == Role::tool) ++end;
        request.messages.erase(begin, end);
        request.messages.back().content = "Earlier conversation was discarded. Summarize the retained conversation above as instructed.";
        estimated = estimator.estimate(request);
    }
    if (estimated > limit || std::none_of(request.messages.begin(), request.messages.end(),
                                          [](const Message& m) { return m.role == Role::assistant; })) {
        throw ModelError(ModelError::Kind::rejected, {}, "summary budget is too small to retain meaningful progress");
    }
    return request;
}

} // namespace

Budget Budget::from(const ContextOptions& options, std::size_t max_tokens) {
    // 小窗口不能发生无符号下溢；不够容纳预留量时可用预算为零。
    const auto available = options.window_tokens - std::min(options.window_tokens,
                                                           options.safety_margin_tokens);
    const auto limit = available - std::min(available, max_tokens);
    const auto percent = [limit](int value) {
        const auto p = static_cast<std::size_t>(std::clamp(value, 0, 100));
        return limit / 100 * p + limit % 100 * p / 100;
    };
    return {limit, percent(options.compaction_trigger_percent),
            percent(options.compaction_target_percent)};
}

std::string texts::pruned_output(std::string_view summary) {
    return std::format("[Old tool output omitted: {}. Call the tool again if needed.]", summary);
}

std::string texts::summary_message(std::string_view summary) {
    return std::format("<summary>\n{}\n</summary>\nThe text above is a summary of the previous conversation; the original messages were compacted. "
                       "Continue the user's task. Read files again when their contents are needed; do not guess file contents from the summary.", summary);
}

Compactor::Compactor(ContextOptions options, std::size_t max_tokens, std::string compact_prompt)
    : budget_(Budget::from(options, max_tokens)), prompt_(std::move(compact_prompt)) {}

std::optional<CompactionChange> Compactor::maybe_compact(const Conversation& c, const RequestShape& s,
                                                         ModelSession& m, TokenEstimator& e,
                                                         const Sink& sink, std::stop_token stop) {
    return compact(Mode::automatic, c, s, m, e, sink, stop);
}

std::optional<CompactionChange> Compactor::force(const Conversation& c, const RequestShape& s,
                                                 ModelSession& m, TokenEstimator& e, const Sink& sink,
                                                 std::stop_token stop) {
    return compact(Mode::forced, c, s, m, e, sink, stop);
}

std::optional<CompactionChange> Compactor::summarize(const Conversation& c, const RequestShape& s,
                                                     ModelSession& m, TokenEstimator& e, const Sink& sink,
                                                     std::stop_token stop) {
    return compact(Mode::manual, c, s, m, e, sink, stop);
}

std::optional<CompactionChange> Compactor::compact(Mode mode, const Conversation& conversation,
                                                   const RequestShape& shape, ModelSession& model,
                                                   TokenEstimator& estimator, const Sink& sink,
                                                   std::stop_token stop) {
    check_stop(stop);
    const auto estimate = [&](const Conversation& c) {
        return estimator.estimate(c.build(shape.system, shape.tools, shape.params, shape.turn_context));
    };
    const auto before = estimate(conversation);
    if (budget_.limit == 0 && !conversation.entries().empty()) {
        throw ModelError(ModelError::Kind::context_too_long, {},
                         "context budget is zero; increase context.window_tokens or reduce reserved tokens");
    }
    if (mode == Mode::automatic && before <= budget_.trigger) return std::nullopt;

    // 摘要取消时连第一级裁剪也回滚；记录只在最终提交时写入。
    Conversation pending = conversation;
    // force 不能依赖已被服务端证伪的窗口；手动压缩也应对低于自动阈值的历史有效。
    const auto target = mode == Mode::automatic ? budget_.target
                                                : std::min(budget_.target, pending.tokens() / 2);
    const auto protection = protected_begin(pending, target);
    std::vector<std::int64_t> pruned;
    std::size_t last_note = 0, minimum_summary_cut = 0;
    for (std::size_t i = 0; i < pending.entries().size(); ++i) {
        const auto& message = pending.entries()[i].message;
        if (message.role == Role::assistant && !message.content.empty()) last_note = i;
    }
    auto after = before;
    if (mode != Mode::manual) {
        for (std::size_t i = 0; i < protection; ++i) {
            if (mode == Mode::automatic && after <= budget_.target) break;
            const Entry& entry = pending.entries()[i];
            if (entry.message.role != Role::tool || entry.pruned || entry.tokens <= 256) continue;
            pruned.push_back(entry.ordinal);
            // 没有后续正文的读取可能只是连续取数；先让摘要接过原文，避免模型反复重读。
            if (i > last_note) minimum_summary_cut = i + 1;
            pending.prune(i, texts::pruned_output(entry.summary));
            after = estimate(pending);
        }
    }

    std::string summary;
    std::int64_t keep_from = -1;
    std::size_t discarded = 0;
    if (mode != Mode::automatic || after > budget_.target || minimum_summary_cut > 0) {
        const auto cut = summary_cut(pending, protection, target, minimum_summary_cut);
        if (cut > 0) {
            try {
                // 第一级尚未提交的裁剪也保留原文供摘要读取；更早已经裁剪的内容不会恢复。
                Request request = summary_request(conversation, cut, shape.params, prompt_,
                                                   estimator, budget_.limit);
                estimator.estimate(request);
                const Reply reply = model.complete(request, [](const StreamEvent&) {},
                                                    [](const RetryInfo&) {}, stop);
                if (reply.usage) estimator.observe_prompt_tokens(reply.usage->prompt);
                check_stop(stop);
                summary = reply.message.content;
                if (summary.empty()) throw ModelError(ModelError::Kind::rejected, {}, "summary is empty");
                keep_from = pending.entries()[cut].ordinal;
                pending.replace_prefix(cut, texts::summary_message(summary));
            } catch (const ModelError& error) {
                // 摘要的 partial 不能作为主对话的 assistant 保存。
                if (error.kind() == ModelError::Kind::cancelled) {
                    throw ModelError(ModelError::Kind::cancelled, {}, "compaction cancelled");
                }
                base::logger("agent")->warn("摘要失败，退化为丢弃旧历史：{}", error.what());
                const bool has_summary = pending.entries().front().ordinal == -1;
                std::size_t drop = 0;
                for (const auto candidate : pending.safe_cuts()) {
                    if (candidate > protection) break;
                    if (!has_summary && pending.entries()[candidate].message.role != Role::user) continue;
                    if (has_summary && candidate == 1) continue;
                    drop = candidate;
                    Conversation reduced = pending;
                    reduced.discard_prefix(drop);
                    if (estimate(reduced) <= target) break;
                }
                if (drop > 0) {
                    keep_from = pending.entries()[drop].ordinal;
                    discarded = drop - (has_summary ? 1 : 0);
                    pending.discard_prefix(drop);
                } else {
                    sink(Notice{Notice::Level::warn, "Summary failed and no history can be safely discarded; start a new session with /new"});
                }
            }
        } else if (mode == Mode::manual) {
            sink(Notice{Notice::Level::info, "No older history to compact"});
        }
    }
    check_stop(stop);
    if (pruned.empty() && keep_from < 0) {
        if (after > budget_.limit) {
            throw ModelError(ModelError::Kind::context_too_long, {},
                             "No history can be safely compacted; start a new session with /new");
        }
        return std::nullopt;
    }
    after = estimate(pending);
    CompactionChange change;
    change.conversation = std::move(pending);
    change.pruned = std::move(pruned);
    change.keep_from = keep_from;
    change.summary = std::move(summary);
    change.discarded = discarded;
    change.before = before;
    change.after = after;
    change.limit = budget_.limit;
    change.mode_label = mode == Mode::automatic ? "自动" : mode == Mode::forced ? "强制" : "手动";
    change.summarized = !change.summary.empty();
    return change;
}

} // namespace dagent::agent
