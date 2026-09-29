#include "app/sandbox_status.hpp"

#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "app/toolchain.hpp"
#include "exec/process.hpp"

namespace dagent::app {
namespace {

namespace fs = std::filesystem;

fs::path executable_dir() {
    std::error_code error;
    const fs::path exe = fs::read_symlink("/proc/self/exe", error);
    if (error) return {};
    return exe.parent_path();
}

bool runnable(const fs::path& file) { return !file.empty() && ::access(file.c_str(), X_OK) == 0; }

std::optional<fs::path> host_program(std::string_view name, std::string_view fallback) {
    if (const auto found = exec::which(name)) return found;
    const fs::path fixed(fallback);
    if (runnable(fixed)) return fixed;
    return std::nullopt;
}

std::string read_text_file(const fs::path& file) {
    std::error_code error;
    if (!fs::exists(file, error)) return {};
    const auto size = fs::file_size(file, error);
    if (error || size > 4096) return {};
    std::string text(size, '\0');
    FILE* handle = ::fopen(file.c_str(), "rb");
    if (!handle) return {};
    const std::size_t read = std::fread(text.data(), 1, size, handle);
    std::fclose(handle);
    text.resize(read);
    return text;
}

nlohmann::json parse_probe_output(std::string_view output) {
    const std::size_t end = output.find_last_not_of(" \t\r\n");
    if (end == std::string_view::npos) return nlohmann::json::object();
    const std::size_t begin = output.rfind('\n', end);
    const std::string_view line = output.substr(begin == std::string_view::npos ? 0 : begin + 1, end - begin);
    try {
        return nlohmann::json::parse(line);
    } catch (...) {
        return nlohmann::json::object();
    }
}

} // namespace

std::optional<exec::SrtRuntime> sandbox_runtime(const HomePaths& paths) {
    Toolchain toolchain(paths);
    const nlohmann::json status = toolchain.status();
    if (!status.value("current", false)) return std::nullopt;
    const nlohmann::json installed = status.value("installed", nlohmann::json::object());
    const nlohmann::json programs = installed.value("programs", nlohmann::json::object());
    const nlohmann::json environments = installed.value("environments", nlohmann::json::object());
    if (!environments.contains("internal/sandbox")) return std::nullopt;

    exec::SrtRuntime runtime;
    runtime.node = programs.value("node", std::string{});
    runtime.shell = programs.value("bash", std::string{});
    runtime.rg = programs.value("rg", std::string{});
    runtime.entry = fs::path(environments.at("internal/sandbox").value("directory", "")) /
                    "node_modules/@anthropic-ai/sandbox-runtime/dist/index.js";
    runtime.bridge = executable_dir() / "libexec/srt_bridge.mjs";
    const auto bwrap = host_program("bwrap", "/usr/bin/bwrap");
    const auto socat = host_program("socat", "/usr/bin/socat");
    if (!bwrap || !socat) return std::nullopt;
    runtime.bwrap = *bwrap;
    runtime.socat = *socat;

    if (!runnable(runtime.node) || !runnable(runtime.shell) || !runnable(runtime.rg) ||
        !fs::exists(runtime.entry) || !fs::exists(runtime.bridge))
        return std::nullopt;
    return runtime;
}

nlohmann::json sandbox_probe(const exec::SrtRuntime& runtime, const fs::path& state_root) {
    const fs::path workdir = state_root / ("probe-" + std::to_string(::getpid()));
    std::error_code error;
    fs::remove_all(workdir, error);
    fs::create_directories(workdir, error);

    exec::Command command;
    command.argv = {runtime.node.string(), runtime.bridge.string(), "--mode=probe",
                    "--srt=" + runtime.entry.string(), "--shell=" + runtime.shell.string(),
                    "--rg=" + runtime.rg.string(), "--bwrap=" + runtime.bwrap.string(),
                    "--socat=" + runtime.socat.string(), "--cwd=" + workdir.string(),
                    "--workdir=" + workdir.string()};
    command.timeout = std::chrono::milliseconds(60000);
    exec::Options options;
    options.default_timeout = std::chrono::milliseconds(60000);
    options.max_output_bytes = 256 << 10;

    nlohmann::json probe;
    try {
        const exec::Result result = exec::run(command, options);
        const nlohmann::json parsed = parse_probe_output(result.out);
        probe = parsed.empty() ? nlohmann::json{{"ok", false},
                                                {"stage", "bridge"},
                                                {"error", result.err.empty() ? result.out : result.err},
                                                {"exit_code", result.exit_code.value_or(-1)},
                                                {"timed_out", result.timed_out}}
                               : parsed;
    } catch (const exec::ExecError& failure) {
        probe = nlohmann::json{{"ok", false}, {"stage", "spawn"}, {"error", failure.what()}};
    }
    fs::remove_all(workdir, error);
    return probe;
}

nlohmann::json sandbox_status(const HomePaths& paths) {
    Toolchain toolchain(paths);
    const nlohmann::json status = toolchain.status();
    const nlohmann::json installed = status.value("installed", nlohmann::json::object());
    const nlohmann::json programs = installed.value("programs", nlohmann::json::object());
    const nlohmann::json environments = installed.value("environments", nlohmann::json::object());
    const std::optional<exec::SrtRuntime> runtime = sandbox_runtime(paths);

    nlohmann::json checks = nlohmann::json::array();
    std::vector<std::string> missing;
    const auto check = [&](std::string name, bool ok, std::string detail) {
        checks.push_back({{"name", std::move(name)}, {"ok", ok}, {"detail", std::move(detail)}});
        if (!ok) missing.push_back(name);
    };

    const bool prepared = status.value("current", false) && installed.is_object();
    check("runtime", prepared, prepared ? "runtime is prepared for the current configuration"
                                        : "run dagent runtime sync");

    const fs::path env = environments.contains("internal/sandbox")
                             ? fs::path(environments.at("internal/sandbox").value("directory", ""))
                             : fs::path{};
    check("sandbox-runtime", runtime.has_value() && !runtime->entry.empty(),
          (env / "node_modules/@anthropic-ai/sandbox-runtime/dist/index.js").string());
    check("bridge", runtime.has_value() && fs::exists(runtime->bridge),
          (executable_dir() / "libexec/srt_bridge.mjs").string());
    check("node", runtime.has_value() && runnable(runtime->node),
          programs.value("node", std::string{"managed node is missing"}));
    check("bash", runtime.has_value() && runnable(runtime->shell),
          programs.value("bash", std::string{"managed bash is missing"}));
    check("rg", runtime.has_value() && runnable(runtime->rg),
          programs.value("rg", std::string{"managed rg is missing"}));
    const auto bwrap = host_program("bwrap", "/usr/bin/bwrap");
    check("bwrap", bwrap.has_value(), bwrap ? bwrap->string() : "not found; install bubblewrap");
    const auto socat = host_program("socat", "/usr/bin/socat");
    check("socat", socat.has_value(), socat ? socat->string() : "not found; install socat");

    std::string joined;
    for (const auto& name : missing) joined += (joined.empty() ? "" : ", ") + name;

    nlohmann::json out;
    out["toolchain_current"] = prepared;
    out["static"] = checks;
    if (!runtime) {
        out["probe"] = nlohmann::json{{"ok", false}, {"stage", "skipped"}, {"error", "missing: " + joined}};
        return out;
    }
    out["probe"] = sandbox_probe(*runtime, paths.runtime / "sandbox");

    const std::string probe_error = out["probe"].value("error", "");
    if (!out["probe"].value("ok", false) &&
        (probe_error.find("namespace") != std::string::npos || probe_error.find("uid_map") != std::string::npos ||
         probe_error.find("uid map") != std::string::npos || probe_error.find("loopback") != std::string::npos)) {
        out["host_prerequisite"] =
            "bubblewrap cannot create a user namespace; an administrator must deploy an AppArmor profile that "
            "grants userns (or an equivalent host policy) for the sandbox stack";
    }
    const std::string restrict = read_text_file("/proc/sys/kernel/apparmor_restrict_unprivileged_userns");
    if (!restrict.empty()) out["host"]["apparmor_restrict_unprivileged_userns"] = std::stoi(restrict);
    return out;
}

} // namespace dagent::app
