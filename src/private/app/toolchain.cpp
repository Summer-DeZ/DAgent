#include "app/toolchain.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <curl/curl.h>
#include <openssl/evp.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace dagent::app {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

std::string read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + file.string());
    return {std::istreambuf_iterator<char>(in), {}};
}

json read_json(const fs::path& file) { return json::parse(read(file), nullptr, true, true); }

std::string digest(std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int length = 0;
    if (EVP_Digest(data.data(), data.size(), bytes.data(), &length, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 failed");
    std::ostringstream out;
    for (unsigned int i = 0; i < length; ++i) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
    return out.str();
}

void write_json(const fs::path& file, const json& value) {
    fs::create_directories(file.parent_path());
    const fs::path temporary = file.string() + ".tmp";
    std::ofstream out(temporary);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out << value.dump(2) << '\n';
    out.close();
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write);
    fs::rename(temporary, file);
}

std::string platform() {
    utsname info{};
    if (::uname(&info) != 0) throw std::runtime_error("cannot determine runtime platform");
    return std::string(info.sysname) + "-" + info.machine;
}

bool identifier(std::string_view value) {
    return !value.empty() && value != "." && value != ".." &&
        std::ranges::all_of(value, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; });
}

fs::path within(const fs::path& root, const fs::path& relative) {
    if (relative.empty() || relative.is_absolute()) throw std::runtime_error("expected a relative runtime path");
    const auto result = (root / relative).lexically_normal();
    const auto part = result.lexically_relative(root);
    if (part.empty() || *part.begin() == "..") throw std::runtime_error("runtime path escapes its directory");
    return result;
}

std::string execute(std::vector<std::string> argv, const fs::path& cwd,
                    const std::vector<std::pair<std::string, std::string>>& env = {}) {
    exec::Command command;
    command.argv = std::move(argv);
    command.cwd = cwd;
    command.inherit_env = false;
    command.env_set = env;
    command.timeout = std::chrono::minutes(20);
    exec::Options options;
    options.max_output_bytes = 65536;
    const auto result = exec::run(command, options);
    if (result.exit_code != 0 || result.timed_out || result.signal)
        throw std::runtime_error(command.argv.front() + " failed: " + result.err + result.out);
    std::string out = result.out;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

void download(const std::string& url, const fs::path& target) {
    if (!url.starts_with("https://")) throw std::runtime_error("runtime downloads require HTTPS");
    std::ofstream out(target, std::ios::binary);
    if (!out) throw std::runtime_error("cannot create download: " + target.string());
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("cannot create runtime download client");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> owner(curl, curl_easy_cleanup);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1200L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* data, std::size_t size, std::size_t count, void* ptr) -> std::size_t {
        auto& stream = *static_cast<std::ofstream*>(ptr);
        stream.write(data, static_cast<std::streamsize>(size * count));
        return stream ? size * count : 0;
    });
    const auto result = curl_easy_perform(curl);
    if (result != CURLE_OK) throw std::runtime_error("runtime download failed: " + std::string(curl_easy_strerror(result)));
}

class SyncLock {
public:
    explicit SyncLock(const fs::path& path) {
        fs::create_directories(path.parent_path());
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd_ < 0 || ::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            if (fd_ >= 0) ::close(fd_);
            throw std::runtime_error("another runtime preparation is active");
        }
    }
    ~SyncLock() { ::close(fd_); }
private:
    int fd_;
};

void replace_root(std::string& value, const fs::path& root) {
    if (value.starts_with("{root}")) value.replace(0, 6, root.string());
}

exec::Environment environment(const HomePaths& paths, const json& installed, std::string_view name) {
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
} // namespace

Toolchain::Toolchain(HomePaths paths) : paths_(std::move(paths)) {
    config_ = read_json(paths_.runtime_config);
    if (config_.value("version", 0) != 1) throw std::runtime_error("unsupported runtime configuration version");
    if (fs::exists(paths_.runtime / "installed.json")) installed_ = read_json(paths_.runtime / "installed.json");
}

static std::string configuration_id(const HomePaths& paths, const json& config) {
    std::string content = config.dump();
    const json environments = config.value("environments", json::object());
    for (const auto& [name, spec] : environments.items()) {
        content += read(within(paths.root, spec.at("lockfile").get<std::string>()));
        if (spec.value("kind", "") == "node")
            content += read(within(paths.root, spec.at("package_json").get<std::string>()));
    }
    return digest(content);
}

json Toolchain::status() const {
    return {{"platform", platform()}, {"configured", config_}, {"installed", installed_},
            {"current", installed_.is_object() && installed_.value("platform", "") == platform() &&
                        installed_.value("configuration", "") == configuration_id(paths_, config_)}};
}

std::map<std::string, exec::Environment> Toolchain::environments() const {
    if (!installed_.is_object() || installed_.value("configuration", "") != configuration_id(paths_, config_) ||
        installed_.value("platform", "") != platform())
        throw std::runtime_error("managed runtime is not prepared for this configuration; run dagent runtime sync");
    for (const auto& [name, file] : installed_.at("programs").items())
        if (::access(file.get<std::string>().c_str(), X_OK) != 0)
            throw std::runtime_error("managed program is missing: " + name + "; run dagent runtime sync");
    std::map<std::string, exec::Environment> out;
    out.emplace("managed", environment(paths_, installed_, "managed"));
    for (const auto& [name, item] : installed_.at("environments").items()) {
        if (!fs::exists(fs::path(item.at("directory").get<std::string>()) / ".ready.json"))
            throw std::runtime_error("managed environment is missing: " + name + "; run dagent runtime sync");
        out.emplace(name, environment(paths_, installed_, name));
    }
    return out;
}

fs::path Toolchain::program(std::string_view name) const {
    return installed_.at("programs").at(std::string(name)).get<std::string>();
}

json Toolchain::sync() {
    SyncLock lock(paths_.run / "runtime.lock");
    fs::create_directories(paths_.cache);
    json next{{"platform", platform()}, {"configuration", configuration_id(paths_, config_)},
              {"packages", json::object()}, {"programs", json::object()}, {"environments", json::object()}};
    const auto& packages = config_.at("packages");
    std::vector<std::string> order;
    for (const auto& [name, package] : packages.items()) if (package.value("kind", "archive") != "python") order.push_back(name);
    for (const auto& [name, package] : packages.items()) if (package.value("kind", "archive") == "python") order.push_back(name);
    for (const auto& name : order) {
        std::cerr << "Preparing runtime: " << name << std::endl;
        const auto& package = packages.at(name);
        const auto version = package.at("version").get<std::string>();
        if (!identifier(name) || !identifier(version)) throw std::runtime_error("invalid runtime package name/version");
        const auto kind = package.value("kind", "archive");
        const fs::path directory = paths_.runtime / name / (version + "-" + digest(package.dump() + platform()).substr(0, 16));
        json entry;
        if (fs::exists(directory / ".ready.json")) entry = read_json(directory / ".ready.json");
        else {
            if (fs::exists(directory)) fs::remove_all(directory); // only an unpublished, incomplete generation
            fs::create_directories(directory);
            entry = {{"version", version}, {"directory", directory.string()}, {"env", json::object()}};
            if (kind == "archive") {
                const auto& asset = package.at("assets").at(platform());
                const auto hash = asset.at("sha256").get<std::string>();
                if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
                    throw std::runtime_error("archive requires a lowercase SHA-256 digest");
                const fs::path archive = paths_.cache / (hash + ".archive");
                if (!fs::exists(archive)) {
                    const fs::path partial = archive.string() + ".part";
                    download(asset.at("url").get<std::string>(), partial);
                    if (digest(read(partial)) != hash) throw std::runtime_error("runtime archive checksum mismatch");
                    fs::rename(partial, archive);
                }
                if (digest(read(archive)) != hash) throw std::runtime_error("cached runtime archive checksum mismatch");
                execute({"/usr/bin/tar", "-xf", archive.string(), "--strip-components=1", "-C", directory.string()}, paths_.root);
                entry["sha256"] = hash;
            } else if (kind == "files") {
                for (const auto& [relative, source] : package.at("files").items()) {
                    const auto target = within(directory, relative);
                    std::string origin = source.get<std::string>();
                    if (origin.starts_with("command:")) {
                        const auto resolved = exec::which(origin.substr(8));
                        if (!resolved) throw std::runtime_error("cannot import tool: " + origin);
                        origin = resolved->string();
                    }
                    fs::create_directories(target.parent_path());
                    fs::copy(fs::canonical(origin), target, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
                }
            } else if (kind == "python") {
                const auto uv = next.at("programs").at("uv").get<std::string>();
                std::vector<std::pair<std::string, std::string>> env = {
                    {"PATH", "/usr/bin:/bin"}, {"HOME", directory.string()}, {"UV_NO_CONFIG", "1"},
                    {"UV_PYTHON_INSTALL_DIR", (directory / "install").string()},
                    {"UV_PYTHON_BIN_DIR", (directory / "bin").string()}, {"UV_CACHE_DIR", (paths_.cache / "uv").string()}};
                execute({uv, "python", "install", version, "--no-bin"}, directory, env);
                const auto python = execute({uv, "python", "find", "--managed-python", "--no-python-downloads", version}, directory, env);
                fs::create_directories(directory / "bin");
                fs::create_symlink(fs::relative(python, directory / "bin"), directory / "bin/python");
                fs::create_symlink("python", directory / "bin/python3");
            } else throw std::runtime_error("unknown runtime package kind: " + kind);
            const auto bin = package.value("bin", "bin");
            entry["bin"] = (bin == "." ? directory : within(directory, bin)).string();
            entry["programs"] = json::object();
            for (const auto& [program, relative] : package.at("programs").items()) {
                const auto file = within(directory, relative.get<std::string>());
                if (::access(file.c_str(), X_OK) != 0) throw std::runtime_error("installed program is not executable: " + file.string());
                entry["programs"][program] = file.string();
                entry["checksums"][program] = digest(read(file));
            }
            const json variables = package.value("env", json::object());
            for (const auto& [key, value] : variables.items()) {
                auto expanded = value.get<std::string>(); replace_root(expanded, directory); entry["env"][key] = expanded;
            }
            write_json(directory / ".ready.json", entry);
        }
        next["packages"][name] = entry;
        for (const auto& [program, file] : entry.at("programs").items()) {
            if (next["programs"].contains(program)) throw std::runtime_error("duplicate managed executable: " + program);
            next["programs"][program] = file;
        }
    }
    for (const auto* name : {"bash", "git", "rg", "uv", "python", "python3", "node", "npm"})
        if (!next["programs"].contains(name)) throw std::runtime_error(std::string("runtime requires package providing ") + name);

    const json environments = config_.value("environments", json::object());
    for (const auto& [name, spec] : environments.items()) {
        std::cerr << "Preparing environment: " << name << std::endl;
        const auto slash = name.find('/');
        if (slash == std::string::npos || (name.substr(0, slash) != "skills" && name.substr(0, slash) != "mcp") ||
            !identifier(name.substr(slash + 1))) throw std::runtime_error("environment name must be skills/<name> or mcp/<name>");
        const auto kind = spec.at("kind").get<std::string>();
        const fs::path lockfile = within(paths_.root, spec.at("lockfile").get<std::string>());
        std::string identity = spec.dump() + read(lockfile) + next["packages"].dump();
        fs::path package_json;
        if (kind == "node") { package_json = within(paths_.root, spec.at("package_json").get<std::string>()); identity += read(package_json); }
        const auto directory = paths_.runtime / "envs" / name / digest(identity).substr(0, 16);
        if (!fs::exists(directory / ".ready.json")) {
            if (fs::exists(directory)) fs::remove_all(directory);
            fs::create_directories(directory);
            auto env = environment(paths_, next, "managed").variables;
            std::erase_if(env, [](const auto& item) { return item.first == "UV_OFFLINE" || item.first == "NPM_CONFIG_OFFLINE"; });
            if (kind == "python") {
                const auto uv = next["programs"]["uv"].get<std::string>();
                execute({uv, "venv", "--python", next["programs"]["python"].get<std::string>(), directory.string()}, paths_.root, env);
                execute({uv, "pip", "sync", "--python", (directory / "bin/python").string(), "--require-hashes", "--only-binary=:all:", lockfile.string()}, paths_.root, env);
            } else if (kind == "node") {
                fs::copy_file(package_json, directory / "package.json");
                fs::copy_file(lockfile, directory / "package-lock.json");
                execute({next["programs"]["npm"].get<std::string>(), "ci", "--no-audit", "--no-fund",
                         spec.value("install_scripts", false) ? "--ignore-scripts=false" : "--ignore-scripts"}, directory, env);
            } else throw std::runtime_error("environment kind must be python or node");
            write_json(directory / ".ready.json", {{"kind", kind}});
        }
        next["environments"][name] = {{"kind", kind}, {"directory", directory.string()}};
    }
    fs::create_directories(paths_.runtime / "homes/managed");
    for (const auto& [name, spec] : next["environments"].items()) fs::create_directories(paths_.runtime / "homes" / name);
    write_json(paths_.runtime / "installed.json", next);
    installed_ = std::move(next);
    return status();
}

} // namespace dagent::app
