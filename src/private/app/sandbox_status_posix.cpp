#include "app/sandbox_status.hpp"
#include "base/platform.hpp"

#include <charconv>
#include <chrono>
#include <fstream>
#include <optional>
#include <string>

#include "exec/process.hpp"

namespace dagent::app::detail {
namespace fs = std::filesystem;
namespace {

/// PATH 查找，找不到时退回 /usr/bin、/usr/sbin 的固定位置。
std::optional<fs::path> host_program(std::string_view name) {
    if (const auto found = exec::which(name)) return found;
    for (const fs::path directory : {"/usr/bin", "/usr/sbin"}) {
        const fs::path fixed = directory / name;
        if (base::can_execute(fixed)) return fixed;
    }
    return std::nullopt;
}

/// `bwrap --version` 的版本号（"bubblewrap 0.6.1" → "0.6.1"）；失败返回空串。
std::string bwrap_version(const fs::path& bwrap) {
    exec::Command command;
    command.argv = {bwrap.string(), "--version"};
    command.timeout = std::chrono::seconds(5);
    try {
        const exec::Result result = exec::run(command);
        if (result.exit_code != 0) return {};
        std::string text = result.out;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
        return text.substr(text.rfind(' ') + 1); // 没有空格时 npos + 1 == 0，取整行
    } catch (const exec::ExecError&) {
        return {};
    }
}

bool version_at_least(std::string_view version, int major, int minor) {
    int found_major = 0, found_minor = 0;
    const char* end = version.data() + version.size();
    auto parsed = std::from_chars(version.data(), end, found_major);
    if (parsed.ec != std::errc{} || parsed.ptr == end || *parsed.ptr != '.') return false;
    if (std::from_chars(parsed.ptr + 1, end, found_minor).ec != std::errc{}) return false;
    return found_major > major || (found_major == major && found_minor >= minor);
}

std::string read_text_file(const fs::path& file) {
    std::ifstream input(file);
    std::string text;
    std::getline(input, text);
    return text;
}

} // namespace

bool platform_sandbox_supported() { return true; }

std::string_view sandbox_containment() { return "bubblewrap"; }

std::vector<HostCheck> host_prerequisites() {
    std::vector<HostCheck> checks;
    if (const auto bwrap = host_program("bwrap")) {
        const std::string version = bwrap_version(*bwrap);
        const bool recent = version_at_least(version, 0, 5);
        checks.push_back({"bwrap", recent, bwrap->string() + " " + (version.empty() ? "unknown version" : version) +
                                               (recent ? "" : "; bubblewrap 0.5 or newer is required")});
    } else {
        checks.push_back({"bwrap", false, "not found; install bubblewrap 0.5 or newer"});
    }
    constexpr std::pair<std::string_view, std::string_view> network_tools[] = {
        {"slirp4netns", "slirp4netns"}, {"unshare", "util-linux"}, {"nsenter", "util-linux"}, {"setsid", "util-linux"},
        {"ip", "iproute2"},             {"iptables-restore", "iptables (nf_tables backend)"}};
    for (const auto& [name, package] : network_tools) {
        const auto program = host_program(name);
        checks.push_back({std::string(name), program.has_value(),
                          program ? program->string() : "not found; install " + std::string(package)});
    }
    return checks;
}

void append_host_diagnostics(nlohmann::json& out) {
    // bwrap 与 unshare（代理网络）在命名空间被宿主限制时报告这些短语。
    const std::string probe_error = out["probe"].value("error", "");
    if (!out["probe"].value("ok", false) &&
        (probe_error.find("namespace") != std::string::npos || probe_error.find("uid_map") != std::string::npos ||
         probe_error.find("uid map") != std::string::npos || probe_error.find("loopback") != std::string::npos ||
         probe_error.find("unshare failed") != std::string::npos)) {
        out["host_prerequisite"] =
            "bubblewrap namespace setup failed; install and load the generated dagent.apparmor profile "
            "for this dagent-backend path, then restart DAgent (see docs/guide/build.md)";
    }
    const std::string restrict = read_text_file("/proc/sys/kernel/apparmor_restrict_unprivileged_userns");
    if (!restrict.empty()) out["host"]["apparmor_restrict_unprivileged_userns"] = std::stoi(restrict);
    out["host"]["apparmor_profile"] = read_text_file("/proc/self/attr/current");
}

} // namespace dagent::app::detail
