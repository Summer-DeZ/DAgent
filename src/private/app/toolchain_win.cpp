#include "app/toolchain_detail.hpp"
#include "base/path.hpp"
#include "base/platform.hpp"
#include "base/security_detail.hpp"

#include <stdexcept>
#include <system_error>
#include <windows.h>

namespace dagent::app::detail {
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {

fs::path windows_directory(UINT (WINAPI *query)(LPWSTR, UINT)) {
    std::wstring buffer(MAX_PATH + 1, L'\0');
    for (;;) {
        const UINT size = query(buffer.data(), static_cast<UINT>(buffer.size()));
        if (!size) throw std::system_error(static_cast<int>(::GetLastError()), std::system_category());
        if (size < buffer.size()) { buffer.resize(size); return fs::path(buffer); }
        buffer.resize(static_cast<std::size_t>(size) + 1);
    }
}

std::string system_path() {
    return base::path_to_utf8(windows_directory(::GetSystemDirectoryW)) + ";" +
           base::path_to_utf8(windows_directory(::GetWindowsDirectoryW));
}

std::vector<std::pair<std::string, std::string>> native_environment() {
    const auto root = windows_directory(::GetWindowsDirectoryW);
    const auto system_directory = windows_directory(::GetSystemDirectoryW);
    const auto temporary = base::path_to_utf8(base::default_temporary_directory());
    // 不继承宿主 PATH/凭据；变量名的大小写合并由 exec 的 Windows 实现负责。
    return {{"SystemRoot", base::path_to_utf8(root)}, {"COMSPEC", base::path_to_utf8(system_directory / L"cmd.exe")},
            {"TEMP", temporary}, {"TMP", temporary}};
}

} // namespace

std::string runtime_platform() { return "Windows-x86_64"; }

void extract_runtime_archive(const fs::path& archive, const fs::path& destination, const fs::path& cwd) {
    const auto tar = windows_directory(::GetSystemDirectoryW) / L"tar.exe";
    auto env = native_environment();
    env.emplace_back("PATH", system_path());
    execute_runtime_command({base::path_to_utf8(tar), "-xf", base::path_to_utf8(archive), "--strip-components=1",
                             "-C", base::path_to_utf8(destination)}, cwd, env);
}

exec::Environment runtime_environment(const HomePaths& paths, const json& installed, std::string_view name) {
    exec::Environment out;
    const fs::path home = paths.runtime / "homes" / base::path_from_utf8(name);
    fs::path root = home;
    std::string path;
    if (name != "managed") {
        const auto& env = installed.at("environments").at(std::string(name));
        root = base::path_from_utf8(env.at("directory").get<std::string>());
        if (env.at("kind") == "python") {
            path += base::path_to_utf8(root / L"Scripts") + ";";
            out.variables.emplace_back("VIRTUAL_ENV", base::path_to_utf8(root));
        }
        path += base::path_to_utf8(root / "node_modules/.bin") + ";";
    }
    for (const auto& [key, package] : installed.at("packages").items()) {
        (void)key;
        path += package.at("bin").get<std::string>() + ";";
        out.readable.push_back(base::path_from_utf8(package.at("directory").get<std::string>()));
        const json variables = package.value("env", json::object());
        for (const auto& [variable, value] : variables.items()) out.variables.emplace_back(variable, value.get<std::string>());
    }
    out.shell = base::path_from_utf8(installed.at("programs").at("bash").get<std::string>());
    out.readable.push_back(root);
    out.variables.insert(out.variables.end(), {
        {"PATH", path + system_path()}, {"HOME", base::path_to_utf8(home)}, {"USERPROFILE", base::path_to_utf8(home)},
        {"XDG_CONFIG_HOME", base::path_to_utf8(home / "config")},
        {"XDG_CACHE_HOME", base::path_to_utf8(paths.cache / base::path_from_utf8(name))},
        {"LC_ALL", "C.UTF-8"}, {"PYTHONNOUSERSITE", "1"}, {"PYTHONDONTWRITEBYTECODE", "1"},
        {"UV_NO_CONFIG", "1"}, {"UV_OFFLINE", "1"}, {"UV_PYTHON_DOWNLOADS", "never"},
        {"UV_CACHE_DIR", base::path_to_utf8(paths.cache / "uv")}, {"NPM_CONFIG_OFFLINE", "true"},
        {"NPM_CONFIG_USERCONFIG", base::path_to_utf8(home / "user.npmrc")},
        {"NPM_CONFIG_GLOBALCONFIG", base::path_to_utf8(home / "global.npmrc")},
        {"NPM_CONFIG_CACHE", base::path_to_utf8(paths.cache / "npm")}});
    const auto native = native_environment();
    out.variables.insert(out.variables.end(), native.begin(), native.end());
    return out;
}

RuntimePrograms install_python_runtime(const fs::path& uv, std::string_view version,
                                       const fs::path& directory, const fs::path& cache) {
    auto env = native_environment();
    env.insert(env.end(), {
        {"PATH", system_path()}, {"HOME", base::path_to_utf8(directory)}, {"USERPROFILE", base::path_to_utf8(directory)},
        {"UV_NO_CONFIG", "1"}, {"UV_PYTHON_INSTALL_DIR", base::path_to_utf8(directory / "install")},
        {"UV_PYTHON_BIN_DIR", base::path_to_utf8(directory / "bin")}, {"UV_CACHE_DIR", base::path_to_utf8(cache / "uv")}});
    execute_runtime_command({base::path_to_utf8(uv), "python", "install", std::string(version), "--no-bin"}, directory, env);
    const auto python = execute_runtime_command({base::path_to_utf8(uv), "python", "find", "--managed-python",
                                                 "--no-python-downloads", std::string(version)}, directory, env);
    const auto interpreter = base::path_from_utf8(python);
    return {{"python", interpreter}, {"python3", interpreter}};
}

fs::path select_python_runtime_program(const fs::path&, const fs::path& installed) { return installed; }

fs::path python_environment_program(const fs::path& environment_root) { return environment_root / L"Scripts\\python.exe"; }

struct SyncLock::Impl {
    HANDLE file = INVALID_HANDLE_VALUE;
    ~Impl() { if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file); }
};

SyncLock::SyncLock(const fs::path& path) : impl_(std::make_unique<Impl>()) {
    fs::create_directories(path.parent_path());
    auto descriptor = base::detail::private_security_descriptor();
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.data(), FALSE};
    impl_->file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &attributes, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    OVERLAPPED range{};
    if (impl_->file == INVALID_HANDLE_VALUE ||
        !::LockFileEx(impl_->file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range))
        throw std::runtime_error("another runtime preparation is active");
}
SyncLock::~SyncLock() = default;

} // namespace dagent::app::detail
