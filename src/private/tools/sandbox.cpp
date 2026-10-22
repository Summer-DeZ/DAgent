#include "tools/detail.hpp"

namespace dagent::tools::detail {

exec::MxcRequest sandbox_request(const Grant& grant, const exec::MxcRuntime& runtime,
    const std::filesystem::path& workspace, const exec::SandboxOptions& options) {
    if (grant.backend != "mxc" || grant.sandbox == agent::SandboxProfile::full_access)
        throw std::runtime_error("restricted execution requires a sandbox grant");
    exec::MxcRequest request;
    request.runtime = runtime;
    request.workspace = workspace;
    auto& policy = request.policy;
    policy.mode = grant.sandbox == agent::SandboxProfile::read_only
        ? exec::Mode::read_only : exec::Mode::workspace_write;
    policy.protect_sensitive_names = grant.protect_sensitive_names;
    policy.readable = grant.readable;
    policy.writable = grant.writable;
    policy.protected_read = grant.protected_read;
    policy.read_exceptions = grant.read_exceptions;
    policy.protected_write = grant.protected_write;
    policy.network_targets = grant.network_targets;
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
