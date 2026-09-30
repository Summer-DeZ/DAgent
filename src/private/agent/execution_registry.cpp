#include "agent/execution_registry.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <utility>

#include "agent/permission.hpp"

namespace dagent::agent {
namespace {

namespace fs = std::filesystem;

int sandbox_rank(SandboxProfile profile) { return static_cast<int>(profile); }

bool contains_all(const std::vector<fs::path>& have, const std::vector<fs::path>& need) {
    for (const fs::path& path : need) {
        const fs::path normalized = path.lexically_normal();
        const bool found = std::ranges::any_of(have, [&](const fs::path& candidate) {
            return candidate.lexically_normal() == normalized;
        });
        if (!found) return false;
    }
    return true;
}

} // namespace

void ActiveExecution::note_network(const NetworkTarget& target, bool persisted) {
    const std::lock_guard lock(mutex_);
    for (auto& [known, known_persisted] : networks_) {
        if (known.host == target.host && known.port == target.port) {
            known_persisted = known_persisted || persisted;
            return;
        }
    }
    networks_.push_back({target, persisted});
}

std::string ActiveExecution::termination_reason() const {
    const std::lock_guard lock(mutex_);
    return termination_reason_;
}

std::shared_ptr<ActiveExecution> ExecutionRegistry::begin(const ToolCall& call,
                                                          const PreparedIntent& intent,
                                                          const ExecutionGrant& grant,
                                                          std::stop_token run_stop) {
    auto execution = std::shared_ptr<ActiveExecution>(new ActiveExecution());
    execution->call = call;
    execution->intent = intent;
    execution->grant = grant;
    if (run_stop.stop_possible()) {
        const std::weak_ptr<ActiveExecution> weak = execution;
        execution->relay_ = std::make_unique<std::stop_callback<std::function<void()>>>(
            run_stop, std::function<void()>([weak] {
                if (const auto locked = weak.lock()) locked->stop.request_stop();
            }));
    }
    const std::lock_guard lock(mutex_);
    active_.push_back(execution);
    return execution;
}

void ExecutionRegistry::end(const std::shared_ptr<ActiveExecution>& execution) {
    if (execution == nullptr) return;
    {
        const std::lock_guard lock(mutex_);
        std::erase(active_, execution);
    }
    execution->relay_.reset();
}

std::vector<std::pair<std::string, std::string>> ExecutionRegistry::reconcile(const Policy& policy) {
    std::vector<std::shared_ptr<ActiveExecution>> snapshot;
    {
        const std::lock_guard lock(mutex_);
        snapshot = active_;
    }
    std::vector<std::pair<std::string, std::string>> stopped;
    for (const auto& execution : snapshot) {
        std::vector<std::pair<NetworkTarget, bool>> networks;
        {
            const std::lock_guard lock(execution->mutex_);
            networks = execution->networks_;
        }
        const Verdict verdict = policy.evaluate(execution->call, execution->intent);
        std::string reason;
        if (verdict.kind == Verdict::Kind::deny) {
            reason = "the current permission no longer allows this call";
        } else if (verdict.kind == Verdict::Kind::ask && execution->grant.source != GrantSource::once) {
            reason = "the authorization for this call was revoked or downgraded";
        } else if (verdict.kind == Verdict::Kind::allow) {
            const ExecutionGrant& next = verdict.grant;
            if (sandbox_rank(next.sandbox) < sandbox_rank(execution->grant.sandbox)) {
                reason = "the sandbox scope was narrowed";
            } else if (!contains_all(next.readable, execution->grant.readable)) {
                reason = "the readable scope was narrowed";
            } else if (execution->intent.kind == ToolKind::write &&
                       !contains_all(next.writable, execution->grant.writable)) {
                reason = "the writable scope was narrowed";
            } else if (execution->grant.allow_network && !next.allow_network && !networks.empty()) {
                reason = "network access was revoked";
            } else {
                for (const auto& [target, persisted] : networks) {
                    if (!persisted) continue;
                    if (policy.check_network(target).kind != Policy::NetworkDecision::Kind::allow) {
                        reason = "a network authorization used by this call was revoked";
                        break;
                    }
                }
            }
        }
        if (reason.empty()) continue;
        {
            const std::lock_guard lock(execution->mutex_);
            if (!execution->termination_reason_.empty()) continue;
            execution->termination_reason_ = reason;
        }
        execution->stop.request_stop();
        stopped.push_back({execution->call.id, reason});
    }
    return stopped;
}

} // namespace dagent::agent
