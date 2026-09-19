#pragma once

#include "agent/conversation.hpp"
#include "agent/options.hpp"
#include "agent/record.hpp"

namespace dagent::agent {

struct RequestShape {
    std::string system;
    std::vector<ToolDef> tools;
    ModelParams params;
};

struct Budget {
    std::size_t limit, trigger, target;
    static Budget from(const ContextOptions&, std::size_t max_tokens);
};

namespace texts {
std::string pruned_output(std::string_view summary);
std::string summary_message(std::string_view summary);
} // namespace texts

class Compactor {
public:
    Compactor(ContextOptions, std::size_t max_tokens, std::string compact_prompt);

    void maybe_compact(Conversation&, const RequestShape&, Model&, TokenEstimator&, Recorder&,
                       const Sink&, std::stop_token);
    void force(Conversation&, const RequestShape&, Model&, TokenEstimator&, Recorder&,
               const Sink&, std::stop_token);
    void summarize(Conversation&, const RequestShape&, Model&, TokenEstimator&, Recorder&,
                   const Sink&, std::stop_token);
    Budget budget() const { return budget_; }

private:
    enum class Mode { automatic, forced, manual };
    void compact(Mode, Conversation&, const RequestShape&, Model&, TokenEstimator&, Recorder&,
                 const Sink&, std::stop_token);

    Budget budget_;
    std::string prompt_;
};

} // namespace dagent::agent
