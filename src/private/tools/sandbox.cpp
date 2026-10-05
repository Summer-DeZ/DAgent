#include "tools/detail.hpp"

namespace dagent::tools::detail {

exec::SrtRequest sandbox_request(const Grant& grant, const exec::SrtRuntime& runtime,
    const std::filesystem::path& workspace, const std::filesystem::path& state_root,
    const exec::SandboxOptions& options) {
    if (grant.backend != "srt" || grant.sandbox == agent::SandboxProfile::full_access)
        throw std::runtime_error("restricted execution requires an SRT grant");
    exec::SrtRequest request;
    request.runtime = runtime;
    request.workspace = workspace;
    request.state_root = state_root;
    auto& policy = request.policy;
    policy.mode = grant.sandbox == agent::SandboxProfile::read_only
        ? exec::Mode::read_only : exec::Mode::workspace_write;
    policy.allow_network = grant.allow_network;
    policy.allow_local_sockets = grant.allow_local_sockets;
    policy.private_tmp = grant.private_tmp;
    policy.protect_sensitive_names = grant.protect_sensitive_names;
    policy.readable = grant.readable;
    policy.writable = grant.writable;
    policy.protected_read = grant.protected_read;
    policy.read_exceptions = grant.read_exceptions;
    policy.protected_write = grant.protected_write;
    policy.network_targets = grant.network_targets;
    request.startup_timeout = options.startup_timeout;
    request.approval_timeout = options.network_approval_timeout;
    request.max_network_requests = options.max_network_requests_per_execution;
    request.network_gate = [gate = grant.network_decider](std::string_view host, int port,
        std::string& reason, std::stop_token stop) {
        if (!gate) {
            reason = "runtime network approval is unavailable in this run";
            return exec::NetworkGateResult::deny;
        }
        switch (gate(agent::NetworkTarget{std::string(host), port}, reason, stop)) {
        case agent::NetworkAction::allow: return exec::NetworkGateResult::allow;
        case agent::NetworkAction::cancel: return exec::NetworkGateResult::cancel;
        case agent::NetworkAction::deny: return exec::NetworkGateResult::deny;
        }
        return exec::NetworkGateResult::deny;
    };
    return request;
}

} // namespace dagent::tools::detail
