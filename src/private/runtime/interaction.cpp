#include "runtime/interaction.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace dagent::runtime {

struct InteractionBroker::Pending {
    std::string id;
    Kind kind = Kind::approval;
    agent::Approval approval;
    agent::Question question;
    std::string session_id;
    std::uint64_t generation = 0;

    bool done = false;
    bool active = false;
    bool cancelled = false;
    agent::Decision decision;
    agent::Answer answer;
    std::condition_variable_any cv;
};

InteractionRequest InteractionBroker::make_event(const std::shared_ptr<Pending>& pending) {
    InteractionRequest event;
    event.id = pending->id;
    event.kind = pending->kind == InteractionBroker::Kind::approval
                     ? InteractionRequest::Kind::approval
                     : InteractionRequest::Kind::question;
    event.approval = pending->approval;
    event.question = pending->question;
    event.session_id = pending->session_id;
    event.generation = pending->generation;
    return event;
}

InteractionBroker::~InteractionBroker() { cancel_all(); }

void InteractionBroker::set_outlet(InteractionOutlet* outlet) {
    const std::lock_guard lock(mutex_);
    outlet_ = outlet;
}

std::shared_ptr<InteractionBroker::Pending> InteractionBroker::complete_locked(
    const std::shared_ptr<Pending>& pending, bool activate_next) {
    const bool was_active = pending->active;
    pending->done = true;
    pending->active = false;
    if (was_active) active_.reset();
    std::erase(waiting_, pending);
    pending->cv.notify_all();
    if (!activate_next || closed_ || !was_active || waiting_.empty()) return nullptr;
    active_ = waiting_.front();
    waiting_.pop_front();
    active_->active = true;
    return active_;
}

void InteractionBroker::cancel_all() {
    std::vector<std::shared_ptr<Pending>> pending;
    {
        const std::lock_guard lock(mutex_);
        closed_ = true;
        if (active_) {
            pending.push_back(active_);
            active_.reset();
        }
        for (const auto& item : waiting_) pending.push_back(item);
        waiting_.clear();
        for (const auto& item : pending) {
            item->done = true;
            item->active = false;
            item->cancelled = true;
            item->cv.notify_all();
        }
    }
    if (outlet_ != nullptr) {
        for (const auto& item : pending) outlet_->interaction_closed(item->id);
    }
}

std::shared_ptr<InteractionBroker::Pending> InteractionBroker::request(
    Kind kind, agent::Approval approval, agent::Question question, std::string session_id,
    std::uint64_t generation, std::stop_token stop) {
    auto pending = std::make_shared<Pending>();
    pending->kind = kind;
    pending->approval = std::move(approval);
    pending->question = std::move(question);
    pending->session_id = std::move(session_id);
    pending->generation = generation;

    std::shared_ptr<Pending> first;
    {
        const std::lock_guard lock(mutex_);
        pending->id = std::format("i-{}", ++next_interaction_);
        if (closed_) {
            pending->done = true;
            pending->cancelled = true;
            return pending;
        }
        if (active_ == nullptr) {
            active_ = pending;
            pending->active = true;
            first = pending;
        } else {
            waiting_.push_back(pending);
        }
    }
    if (first != nullptr && outlet_ != nullptr) outlet_->interaction_requested(make_event(first));

    std::shared_ptr<Pending> next;
    {
        std::unique_lock lock(mutex_);
        pending->cv.wait(lock, stop, [&] { return pending->done; });
        if (!pending->done) {
            pending->cancelled = true;
            next = complete_locked(pending, true);
        }
    }
    if (next != nullptr && outlet_ != nullptr) outlet_->interaction_requested(make_event(next));
    if (pending->cancelled && outlet_ != nullptr) outlet_->interaction_closed(pending->id);
    return pending;
}

agent::Decision InteractionBroker::request_approval(agent::Approval approval, std::string session_id,
                                                    std::uint64_t generation, std::stop_token stop) {
    const std::shared_ptr<Pending> pending =
        request(Kind::approval, std::move(approval), {}, std::move(session_id), generation, stop);
    if (pending->cancelled) return agent::Decision{};
    return pending->decision;
}

agent::Answer InteractionBroker::request_answer(agent::Question question, std::string session_id,
                                                std::uint64_t generation, std::stop_token stop) {
    const std::shared_ptr<Pending> pending =
        request(Kind::question, {}, std::move(question), std::move(session_id), generation, stop);
    if (pending->cancelled) return agent::Answer{{}, {}, true};
    return pending->answer;
}

bool InteractionBroker::answer_approval(const std::string& interaction_id, agent::Decision decision) {
    std::shared_ptr<Pending> next;
    bool accepted = false;
    {
        const std::lock_guard lock(mutex_);
        std::shared_ptr<Pending> target;
        if (active_ != nullptr && active_->id == interaction_id) target = active_;
        if (target == nullptr) {
            const auto it = std::ranges::find_if(waiting_, [&](const std::shared_ptr<Pending>& item) {
                return item->id == interaction_id;
            });
            if (it != waiting_.end()) target = *it;
        }
        if (target != nullptr && !target->done && target->kind == Kind::approval) {
            target->decision = std::move(decision);
            next = complete_locked(target, true);
            accepted = true;
        }
    }
    if (next != nullptr && outlet_ != nullptr) outlet_->interaction_requested(make_event(next));
    return accepted;
}

bool InteractionBroker::answer_question(const std::string& interaction_id, agent::Answer answer) {
    std::shared_ptr<Pending> next;
    bool accepted = false;
    {
        const std::lock_guard lock(mutex_);
        std::shared_ptr<Pending> target;
        if (active_ != nullptr && active_->id == interaction_id) target = active_;
        if (target == nullptr) {
            const auto it = std::ranges::find_if(waiting_, [&](const std::shared_ptr<Pending>& item) {
                return item->id == interaction_id;
            });
            if (it != waiting_.end()) target = *it;
        }
        if (target != nullptr && !target->done && target->kind == Kind::question) {
            target->answer = std::move(answer);
            next = complete_locked(target, true);
            accepted = true;
        }
    }
    if (next != nullptr && outlet_ != nullptr) outlet_->interaction_requested(make_event(next));
    return accepted;
}

} // namespace dagent::runtime
