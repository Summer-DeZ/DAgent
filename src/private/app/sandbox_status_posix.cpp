#include "app/sandbox_status.hpp"
#include "base/platform.hpp"

#include <fstream>
#include <string>

#include "exec/process.hpp"

namespace dagent::app::detail {
namespace fs = std::filesystem;
namespace {

std::optional<fs::path> host_program(std::string_view name, std::string_view fallback) {
    if (const auto found = exec::which(name)) return found;
    const fs::path fixed(fallback);
    if (!fixed.empty() && base::can_execute(fixed)) return fixed;
    return std::nullopt;
}

std::string read_text_file(const fs::path& file) {
    std::ifstream input(file);
    std::string text;
    std::getline(input, text);
    return text;
}

} // namespace

bool platform_sandbox_supported() { return true; }

std::optional<fs::path> host_bwrap() { return host_program("bwrap", "/usr/bin/bwrap"); }
std::optional<fs::path> host_socat() { return host_program("socat", "/usr/bin/socat"); }

void append_host_diagnostics(nlohmann::json& out) {
    const std::string probe_error = out["probe"].value("error", "");
    if (!out["probe"].value("ok", false) &&
        (probe_error.find("namespace") != std::string::npos || probe_error.find("uid_map") != std::string::npos ||
         probe_error.find("uid map") != std::string::npos || probe_error.find("loopback") != std::string::npos)) {
        out["host_prerequisite"] =
            "bubblewrap namespace setup failed; install and load the generated dagent.apparmor profile "
            "for this dagent-backend path, then restart DAgent (see docs/guide/build.md)";
    }
    const std::string restrict = read_text_file("/proc/sys/kernel/apparmor_restrict_unprivileged_userns");
    if (!restrict.empty()) out["host"]["apparmor_restrict_unprivileged_userns"] = std::stoi(restrict);
    out["host"]["apparmor_profile"] = read_text_file("/proc/self/attr/current");
}

} // namespace dagent::app::detail
