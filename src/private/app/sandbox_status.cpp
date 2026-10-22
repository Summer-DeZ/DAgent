#include "app/sandbox_status.hpp"
#include "base/platform.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/toolchain.hpp"
#include "base/text.hpp"
#include "exec/process.hpp"

namespace dagent::app {
namespace {

namespace fs = std::filesystem;

bool runnable(const fs::path& file) { return !file.empty() && base::can_execute(file); }

/// runtime 中与 MXC 后端相关的各项；sandbox_runtime 汇总，sandbox_status 分项报告。
struct Resolved {
    bool prepared = false;
    fs::path library_file;
    std::shared_ptr<const exec::MxcLibrary> library;
    std::string library_error;
    fs::path shell, rg;
    nlohmann::json tier;    ///< Windows 请求探测的结果；库没有探测入口时为 null
    std::string tier_error; ///< 层级不是 base-container 或探测失败的原因
};

/// 两次真实启动共用的最小请求：只读模式、托管 bash 所在目录为工作目录（本就在可读范围内），不写工作目录。
exec::MxcRequest probe_request(const exec::MxcRuntime& runtime) {
    exec::MxcRequest request;
    request.runtime = runtime;
    request.workspace = runtime.shell.parent_path();
    request.policy.mode = exec::Mode::read_only;
    request.policy.readable = {request.workspace};
    request.environment = {{"PATH", exec::default_search_path()}, {"LC_ALL", "C.UTF-8"}};
    request.timeout = std::chrono::seconds(30);
    return request;
}

/// Windows 只接受 ProcessContainer 的 base-container 层：用与代理网络探测同形的请求询问 MXC 的层级选择，
/// 其它层（AppContainer + BFS / DACL）按无沙箱处理。其它平台的库没有探测入口，不做层级判断。
void check_tier(Resolved& out) {
    auto request = probe_request(exec::MxcRuntime{out.library, std::string(detail::sandbox_containment()), out.shell, out.rg});
    request.network_gate = [](std::string_view, int, std::string&, std::stop_token) { return exec::NetworkGateResult::deny; };
    try {
        const auto probe = exec::probe_mxc(request);
        if (!probe) return;
        out.tier = *probe;
        const std::string tier = probe->value("tier", "");
        if (tier != "base-container")
            out.tier_error = std::format("MXC selected the ProcessContainer tier '{}'; DAgent accepts only 'base-container', "
                                         "which requires Windows 11 26H2 (build 26300) or newer",
                                         tier.empty() ? "unknown" : tier);
    } catch (const exec::ExecError& error) {
        out.tier_error = std::string("MXC tier probe failed: ") + error.what();
    }
}

Resolved resolve(const HomePaths& paths) {
    Resolved out;
    Toolchain toolchain(paths);
    out.prepared = toolchain.status().value("current", false);
    if (!out.prepared) return out;
    out.shell = toolchain.program("bash");
    out.rg = toolchain.program("rg");
    out.library_file = toolchain.library("mxc");
    // 没有后端的平台不加载原生库。
    if (!detail::platform_sandbox_supported() || out.library_file.empty() || !fs::exists(out.library_file))
        return out;
    try {
        out.library = exec::MxcLibrary::load(out.library_file);
    } catch (const exec::ExecError& error) {
        out.library_error = error.what();
        return out;
    }
    check_tier(out);
    return out;
}

std::expected<exec::MxcRuntime, std::string> to_runtime(const Resolved& resolved) {
    if (!resolved.prepared)
        return std::unexpected("the managed runtime is not prepared; run `dagent runtime sync`");
    if (resolved.library_file.empty() || !fs::exists(resolved.library_file))
        return std::unexpected("the MXC library is not installed; run `dagent runtime sync`");
    if (!resolved.library) return std::unexpected(resolved.library_error);
    if (!resolved.tier_error.empty()) return std::unexpected(resolved.tier_error);
    if (!runnable(resolved.shell) || !runnable(resolved.rg))
        return std::unexpected("managed bash or rg is missing; run `dagent runtime sync`");
    return exec::MxcRuntime{resolved.library, std::string(detail::sandbox_containment()), resolved.shell, resolved.rg};
}

/// 跑一次探测命令；成功（退出 0 且输出符合）返回 nullopt，否则返回失败报告。
std::optional<nlohmann::json> probe_step(std::string_view stage, const exec::MxcRequest& request,
                                         std::string_view expected, nlohmann::json& elapsed) {
    exec::Options options;
    options.max_output_bytes = 64 << 10;
    try {
        const exec::Result result = exec::run_mxc(request, options);
        elapsed[std::string(stage)] = result.elapsed.count();
        if (result.exit_code == 0 && !result.timed_out && result.out == expected) return std::nullopt;
        // 失败输出常带缩进与结尾换行（如 MSYS2 的 fatal 行）；状态报告与欢迎页只要正文。
        std::string error = base::to_valid_utf8(result.err.empty() ? result.out : result.err);
        error.erase(0, error.find_first_not_of(" \t\r\n"));
        error.erase(error.find_last_not_of(" \t\r\n") + 1);
        return nlohmann::json{{"ok", false},
                              {"stage", stage},
                              {"error", error},
                              {"exit_code", result.exit_code.value_or(-1)},
                              {"timed_out", result.timed_out}};
    } catch (const exec::ExecError& failure) {
        return nlohmann::json{{"ok", false}, {"stage", stage}, {"error", failure.what()}};
    }
}

} // namespace

std::expected<exec::MxcRuntime, std::string> sandbox_runtime(const HomePaths& paths) {
    if (!detail::platform_sandbox_supported()) return std::unexpected("this platform has no sandbox backend");
    return to_runtime(resolve(paths));
}

nlohmann::json sandbox_probe(const exec::MxcRuntime& runtime) {
    exec::MxcRequest request = probe_request(runtime);
    nlohmann::json elapsed = nlohmann::json::object();

    request.command = R"(printf ok > "$HOME/probe.txt" && cat "$HOME/probe.txt")";
    if (auto failed = probe_step("filesystem", request, "ok", elapsed)) return *failed;

    request.command = "true";
    request.network_gate = [](std::string_view, int, std::string& reason, std::stop_token) {
        reason = "the sandbox probe allows no network target";
        return exec::NetworkGateResult::deny;
    };
    if (auto failed = probe_step("network", request, "", elapsed)) return *failed;
    return {{"ok", true}, {"stage", "completed"}, {"mxc", runtime.library->version()}, {"elapsed_ms", elapsed}};
}

nlohmann::json sandbox_status(const HomePaths& paths) {
    nlohmann::json checks = nlohmann::json::array();
    std::vector<std::string> missing;
    const auto check = [&](std::string name, bool ok, std::string detail) {
        if (!ok) missing.push_back(name);
        checks.push_back({{"name", std::move(name)}, {"ok", ok}, {"detail", std::move(detail)}});
    };

    const Resolved resolved = resolve(paths);
    check("runtime", resolved.prepared, resolved.prepared ? "runtime is prepared for the current configuration"
                                                          : "run dagent runtime sync");
    if (!detail::platform_sandbox_supported()) {
        // 没有后端的平台只报告 runtime 状态；不列宿主依赖、不启动探测、不读宿主诊断。
        nlohmann::json out;
        out["toolchain_current"] = resolved.prepared;
        out["static"] = checks;
        out["probe"] = nlohmann::json{{"ok", false}, {"stage", "unsupported"},
                                      {"error", "this platform has no sandbox backend"}};
        return out;
    }

    const bool installed = !resolved.library_file.empty() && fs::exists(resolved.library_file);
    check("mxc-library", installed, installed ? resolved.library_file.string() : "not installed; run dagent runtime sync");
    if (resolved.library)
        check("mxc-load", true, "version " + resolved.library->version());
    else
        check("mxc-load", false, installed ? resolved.library_error
                                           : "requires MXC " + std::string(exec::kMxcVersion));
    if (!resolved.tier.is_null() || !resolved.tier_error.empty())
        check("mxc-tier", resolved.tier_error.empty(),
              resolved.tier_error.empty() ? resolved.tier.value("tier", "") : resolved.tier_error);
    check("bash", runnable(resolved.shell),
          resolved.shell.empty() ? "managed bash is missing" : resolved.shell.string());
    check("rg", runnable(resolved.rg), resolved.rg.empty() ? "managed rg is missing" : resolved.rg.string());
    for (auto& host : detail::host_prerequisites()) check(std::move(host.name), host.ok, std::move(host.detail));

    std::string joined;
    for (const auto& name : missing) joined += (joined.empty() ? "" : ", ") + name;

    nlohmann::json out;
    out["toolchain_current"] = resolved.prepared;
    out["static"] = checks;
    if (resolved.library) {
        out["platform_support"] = resolved.library->platform_support();
        out["available_backends"] = resolved.library->available_backends();
    }
    if (!resolved.tier.is_null()) out["tier_probe"] = resolved.tier;
    const auto runtime = to_runtime(resolved);
    if (!runtime) {
        out["probe"] = nlohmann::json{{"ok", false}, {"stage", "skipped"}, {"error", "missing: " + joined}};
        return out;
    }
    out["probe"] = sandbox_probe(*runtime);

    detail::append_host_diagnostics(out);
    return out;
}

} // namespace dagent::app
