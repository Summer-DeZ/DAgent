#include "app/toolchain_detail.hpp"

#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace dagent::app::detail {
namespace fs = std::filesystem;
using json = nlohmann::json;

std::string runtime_platform() {
    utsname info{};
    if (::uname(&info) != 0) throw std::runtime_error("cannot determine runtime platform");
    return std::string(info.sysname) + "-" + info.machine;
}

void extract_runtime_archive(const fs::path& archive, const fs::path& destination,
                             const fs::path& cwd) {
    execute_runtime_command({"/usr/bin/tar", "-xf", archive.string(), "--strip-components=1", "-C", destination.string()}, cwd);
}

exec::Environment runtime_environment(const HomePaths& paths, const json& installed, std::string_view name) {
    exec::Environment out;
    std::string path;
    fs::path root = paths.runtime / "homes" / name;
    if (name != "managed") {
        const auto& env = installed.at("environments").at(std::string(name));
        root = env.at("directory").get<std::string>();
        path = root.string() + "/bin:" + root.string() + "/node_modules/.bin:";
        if (env.at("kind") == "python") out.variables.emplace_back("VIRTUAL_ENV", root.string());
    }
    for (const auto& [key, package] : installed.at("packages").items()) {
        (void)key;
        path += package.at("bin").get<std::string>() + ":";
        out.readable.emplace_back(package.at("directory").get<std::string>());
        const json variables = package.value("env", json::object());
        for (const auto& [key, value] : variables.items())
            out.variables.emplace_back(key, value.get<std::string>());
    }
    out.shell = installed.at("programs").at("bash").get<std::string>();
    out.readable.push_back(root);
    out.variables.insert(out.variables.end(), {
        {"PATH", path + "/usr/bin:/bin"}, {"HOME", (paths.runtime / "homes" / name).string()},
        {"XDG_CONFIG_HOME", (paths.runtime / "homes" / name / "config").string()},
        {"XDG_CACHE_HOME", (paths.cache / std::string(name)).string()},
        {"LC_ALL", "C.UTF-8"}, {"PYTHONNOUSERSITE", "1"}, {"PYTHONDONTWRITEBYTECODE", "1"},
        {"UV_NO_CONFIG", "1"}, {"UV_OFFLINE", "1"}, {"UV_PYTHON_DOWNLOADS", "never"},
        {"UV_CACHE_DIR", (paths.cache / "uv").string()}, {"NPM_CONFIG_OFFLINE", "true"},
        {"NPM_CONFIG_USERCONFIG", (paths.runtime / "homes" / name / "user.npmrc").string()},
        {"NPM_CONFIG_GLOBALCONFIG", (paths.runtime / "homes" / name / "global.npmrc").string()},
        {"NPM_CONFIG_CACHE", (paths.cache / "npm").string()}});
    return out;
}

void install_python_runtime(const fs::path& uv, std::string_view version,
                            const fs::path& directory, const fs::path& cache) {
    std::vector<std::pair<std::string, std::string>> env = {
        {"PATH", "/usr/bin:/bin"}, {"HOME", directory.string()}, {"UV_NO_CONFIG", "1"},
        {"UV_PYTHON_INSTALL_DIR", (directory / "install").string()},
        {"UV_PYTHON_BIN_DIR", (directory / "bin").string()}, {"UV_CACHE_DIR", (cache / "uv").string()}};
    execute_runtime_command({uv.string(), "python", "install", std::string(version), "--no-bin"}, directory, env);
    const auto python = execute_runtime_command({uv.string(), "python", "find", "--managed-python", "--no-python-downloads", std::string(version)}, directory, env);
    fs::create_directories(directory / "bin");
    fs::create_symlink(fs::relative(python, directory / "bin"), directory / "bin/python");
    fs::create_symlink("python", directory / "bin/python3");
}

fs::path python_environment_program(const fs::path& environment_root) {
    return environment_root / "bin/python";
}

struct SyncLock::Impl {
    int fd_;

    explicit Impl(const fs::path& path) {
        fs::create_directories(path.parent_path());
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd_ < 0 || ::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            if (fd_ >= 0) ::close(fd_);
            throw std::runtime_error("another runtime preparation is active");
        }
    }
    ~Impl() { ::close(fd_); }
};

SyncLock::SyncLock(const fs::path& path) : impl_(std::make_unique<Impl>(path)) {}
SyncLock::~SyncLock() = default;

} // namespace dagent::app::detail
