/// @file parent_review.hpp
/// @brief Dedicated tool-free parent decisions, serialized by the parent Dispatcher mailbox.
#pragma once

#include "agent/execution_mailbox.hpp"
#include "agent/session.hpp"

namespace dagent::agent {

class ParentReviewer {
public:
    ParentReviewer(Session& session, Run& run, const RunServices& services)
        : session_(session), run_(run), services_(services) {}

    /// Child thread posts one request and waits only for its own terminal reply.
    Decision submit(ExecutionMailbox&, Approval&, std::stop_token);

private:
    Decision review(const Approval&, std::stop_token);
    Session& session_;
    Run& run_;
    const RunServices& services_;
};

} // namespace dagent::agent
