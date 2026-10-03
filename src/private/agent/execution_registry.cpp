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

std::vector<NetworkTarget> ActiveExecution::network_targets() const {
    const std::lock_guard lock(mutex_);
    std::vector<NetworkTarget> targets;
    targets.reserve(networks_.size());
    for (const auto& [target, persisted] : networks_) targets.push_back(target);
    return targets;
}

void ActiveExecution::note_authority(std::stop_token authority_stop) {
    if (!authority_stop.stop_possible()) return;
    auto callback = std::make_unique<std::stop_callback<std::function<void()>>>(
        authority_stop, std::function<void()>([weak = weak_from_this()] {
            if (const auto execution = weak.lock()) {
                {
                    const std::lock_guard lock(execution->mutex_);
                    if (execution->termination_reason_.empty())
                        execution->termination_reason_ = "parent permission authority was revoked";
                }
                execution->stop.request_stop();
            }
        }));
    {
        const std::lock_guard lock(mutex_);
        authority_relay_.swap(callback);
    }
    // 旧 callback 析构可能等待正在执行的回调，不能持有该回调也要获取的 mutex_。
}

std::string ActiveExecution::termination_reason() const {
    const std::lock_guard lock(mutex_);
    return termination_reason_;
}

void ActiveExecution::note_termination(std::string reason) {
    const std::lock_guard lock(mutex_);
    if (termination_reason_.empty()) termination_reason_ = std::move(reason);
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
    execution->note_authority(grant.authority_stop);
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
    std::unique_ptr<std::stop_callback<std::function<void()>>> authority;
    {
        const std::lock_guard lock(execution->mutex_);
        authority = std::move(execution->authority_relay_);
    }
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
