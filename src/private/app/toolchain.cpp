#include "app/toolchain.hpp"
#include "app/toolchain_detail.hpp"
#include "base/platform.hpp"
#include "base/path.hpp"

#include "app/hash.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <curl/curl.h>

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

void write_json(const fs::path& file, const json& value) {
    fs::create_directories(file.parent_path());
    const fs::path temporary = file.string() + ".tmp";
    std::ofstream out(temporary);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out << value.dump(2) << '\n';
    out.close();
    base::make_file_private(temporary);
    fs::rename(temporary, file);
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

void download(const std::string& url, const fs::path& target) {
    if (!url.starts_with("https://")) throw std::runtime_error("runtime downloads require HTTPS");
    std::ofstream out(target, std::ios::binary);
    if (!out) throw std::runtime_error("cannot create download: " + target.string());
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("cannot create runtime download client");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> owner(curl, curl_easy_cleanup);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT)); // 同 net/http.cpp
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

void unpack_archive(const json& asset, const HomePaths& paths, const fs::path& directory) {
                const auto hash = asset.at("sha256").get<std::string>();
                if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
                    throw std::runtime_error("archive requires a lowercase SHA-256 digest");
                const fs::path archive = paths.cache / (hash + ".archive");
                if (!fs::exists(archive)) {
                    const fs::path partial = archive.string() + ".part";
                    download(asset.at("url").get<std::string>(), partial);
                    if (sha256(read(partial)) != hash) throw std::runtime_error("runtime archive checksum mismatch");
                    fs::rename(partial, archive);
                }
                if (sha256(read(archive)) != hash) throw std::runtime_error("cached runtime archive checksum mismatch");
                detail::extract_runtime_archive(archive, directory, paths.root, asset.value("strip", 1));
}

void apply_platform_overrides(json& entries) {
    const std::string platform = detail::runtime_platform();
    for (auto item = entries.begin(); item != entries.end();) {
        if (!item->contains("platforms")) {
            ++item;
            continue;
        }
        json platforms = std::move(item->at("platforms"));
        if (!platforms.contains(platform)) {
            item->erase("platforms");
            ++item;
            continue;
        }
        json value = std::move(platforms.at(platform));
        if (value.is_null()) {
            item = entries.erase(item);
            continue;
        }
        *item = std::move(value);
        ++item;
    }
}

void replace_root(std::string& value, const fs::path& root) {
    if (value.starts_with("{root}")) value.replace(0, 6, root.string());
}

} // namespace

std::string detail::execute_runtime_command(
    std::vector<std::string> argv, const fs::path& cwd,
    const std::vector<std::pair<std::string, std::string>>& env) {
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

Toolchain::Toolchain(HomePaths paths) : paths_(std::move(paths)) {
    config_ = read_json(paths_.runtime_config);
    if (config_.value("version", 0) != 1) throw std::runtime_error("unsupported runtime configuration version");
    apply_platform_overrides(config_.at("packages"));
    if (config_.contains("environments")) apply_platform_overrides(config_.at("environments"));
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
    return sha256(content);
}

json Toolchain::status() const {
    return {{"platform", detail::runtime_platform()}, {"configured", config_}, {"installed", installed_},
            {"current", installed_.is_object() && installed_.value("platform", "") == detail::runtime_platform() &&
                        installed_.value("configuration", "") == configuration_id(paths_, config_)}};
}

std::map<std::string, exec::Environment> Toolchain::environments() const {
    if (!installed_.is_object() || installed_.value("configuration", "") != configuration_id(paths_, config_) ||
        installed_.value("platform", "") != detail::runtime_platform())
        throw std::runtime_error("managed runtime is not prepared for this configuration; run dagent runtime sync");
    for (const auto& [name, file] : installed_.at("programs").items())
        if (!base::can_execute(file.get<std::string>()))
            throw std::runtime_error("managed program is missing: " + name + "; run dagent runtime sync");
    std::map<std::string, exec::Environment> out;
    out.emplace("managed", detail::runtime_environment(paths_, installed_, "managed"));
    for (const auto& [name, item] : installed_.at("environments").items()) {
        if (!fs::exists(fs::path(item.at("directory").get<std::string>()) / ".ready.json"))
            throw std::runtime_error("managed environment is missing: " + name + "; run dagent runtime sync");
        out.emplace(name, detail::runtime_environment(paths_, installed_, name));
    }
    return out;
}

fs::path Toolchain::environment_root(std::string_view name) const {
    if (!installed_.is_object() || !installed_.at("environments").contains(std::string(name))) return {};
    return installed_.at("environments").at(std::string(name)).at("directory").get<std::string>();
}

fs::path Toolchain::program(std::string_view name) const {
    return installed_.at("programs").at(std::string(name)).get<std::string>();
}

fs::path Toolchain::library(std::string_view name) const {
    if (!installed_.is_object() || !installed_.contains("libraries") ||
        !installed_.at("libraries").contains(std::string(name)))
        return {};
    return base::path_from_utf8(installed_.at("libraries").at(std::string(name)).get<std::string>());
}

json Toolchain::sync() {
    detail::SyncLock lock(paths_.run / "runtime.lock");
    fs::create_directories(paths_.cache);
    json next{{"platform", detail::runtime_platform()}, {"configuration", configuration_id(paths_, config_)},
              {"packages", json::object()}, {"programs", json::object()}, {"libraries", json::object()},
              {"environments", json::object()}};
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
        const fs::path directory = kind == "system"
            ? base::path_from_utf8(package.at("root").get<std::string>())
            : paths_.runtime / name / (version + "-" + sha256(package.dump() + detail::runtime_platform()).substr(0, 16));
        json entry;
        if (kind != "system" && fs::exists(directory / ".ready.json")) entry = read_json(directory / ".ready.json");
        else {
            if (kind == "system") {
                if (!fs::exists(directory))
                    throw std::runtime_error("system package root is missing: " + package.at("root").get<std::string>());
            } else {
                if (fs::exists(directory)) fs::remove_all(directory); // only an unpublished, incomplete generation
                fs::create_directories(directory);
            }
            detail::RuntimePrograms python_programs;
            entry = {{"version", version}, {"directory", directory.string()}, {"env", json::object()}};
            if (kind == "archive") {
                const auto& asset = package.at("assets").at(detail::runtime_platform());
                unpack_archive(asset, paths_, directory);
                entry["sha256"] = asset.at("sha256");
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
                python_programs = detail::install_python_runtime(base::path_from_utf8(uv), version, directory, paths_.cache);
            } else if (kind != "system") throw std::runtime_error("unknown runtime package kind: " + kind);
            const auto bin = package.value("bin", "bin");
            entry["bin"] = (bin == "." ? directory : within(directory, bin)).string();
            entry["programs"] = json::object();
            const json programs = package.value("programs", json::object());
            for (const auto& [program, relative] : programs.items()) {
                auto file = within(directory, relative.get<std::string>());
                if (const auto found = python_programs.find(program); found != python_programs.end())
                    file = detail::select_python_runtime_program(file, found->second);
                if (!base::can_execute(file)) throw std::runtime_error("installed program is not executable: " + file.string());
                entry["programs"][program] = base::path_to_utf8(file);
                entry["checksums"][program] = sha256(read(file));
            }
            // 原生库只登记路径，不要求可执行位；由使用方在运行时加载。
            entry["libraries"] = json::object();
            const json libraries = package.value("libraries", json::object());
            for (const auto& [library, relative] : libraries.items()) {
                const auto file = within(directory, relative.get<std::string>());
                if (!fs::is_regular_file(file)) throw std::runtime_error("installed library is missing: " + file.string());
                entry["libraries"][library] = base::path_to_utf8(file);
            }
            const json variables = package.value("env", json::object());
            for (const auto& [key, value] : variables.items()) {
                auto expanded = value.get<std::string>(); replace_root(expanded, directory); entry["env"][key] = expanded;
            }
            if (kind != "system") write_json(directory / ".ready.json", entry);
        }
        next["packages"][name] = entry;
        for (const auto& [program, file] : entry.at("programs").items()) {
            if (next["programs"].contains(program)) throw std::runtime_error("duplicate managed executable: " + program);
            next["programs"][program] = file;
        }
        const json libraries = entry.value("libraries", json::object());
        for (const auto& [library, file] : libraries.items()) {
            if (next["libraries"].contains(library)) throw std::runtime_error("duplicate managed library: " + library);
            next["libraries"][library] = file;
        }
    }
    for (const auto* name : {"bash", "git", "rg", "uv", "python", "python3", "node", "npm"})
        if (!next["programs"].contains(name)) throw std::runtime_error(std::string("runtime requires package providing ") + name);

    const json environments = config_.value("environments", json::object());
    for (const auto& [name, spec] : environments.items()) {
        std::cerr << "Preparing environment: " << name << std::endl;
        const auto slash = name.find('/');
        const std::string_view group = slash == std::string::npos ? std::string_view{} : std::string_view(name).substr(0, slash);
        if (group != "skills" && group != "mcp" && group != "internal")
            throw std::runtime_error("environment name must be skills/<name>, mcp/<name> or internal/<name>");
        if (slash == std::string::npos || !identifier(name.substr(slash + 1)))
            throw std::runtime_error("invalid environment name: " + name);
        const auto kind = spec.at("kind").get<std::string>();
        const fs::path lockfile = within(paths_.root, spec.at("lockfile").get<std::string>());
        std::string identity = spec.dump() + read(lockfile) + next["packages"].dump();
        fs::path package_json;
        if (kind == "node") { package_json = within(paths_.root, spec.at("package_json").get<std::string>()); identity += read(package_json); }
        const auto directory = paths_.runtime / "envs" / name / sha256(identity).substr(0, 16);
        if (!fs::exists(directory / ".ready.json")) {
            if (fs::exists(directory)) fs::remove_all(directory);
            fs::create_directories(directory);
            auto env = detail::runtime_environment(paths_, next, "managed").variables;
            std::erase_if(env, [](const auto& item) { return item.first == "UV_OFFLINE" || item.first == "NPM_CONFIG_OFFLINE"; });
            if (kind == "python") {
                const auto uv = next["programs"]["uv"].get<std::string>();
                detail::execute_runtime_command({uv, "venv", "--python", next["programs"]["python"].get<std::string>(), directory.string()}, paths_.root, env);
                detail::execute_runtime_command({uv, "pip", "sync", "--python", detail::python_environment_program(directory).string(), "--require-hashes", "--only-binary=:all:", lockfile.string()}, paths_.root, env);
            } else if (kind == "node") {
                fs::copy_file(package_json, directory / "package.json");
                fs::copy_file(lockfile, directory / "package-lock.json");
                const auto npm = next["programs"]["npm"].get<std::string>();
                std::vector<std::string> command = npm.ends_with(".js")
                    ? std::vector<std::string>{next["programs"]["node"].get<std::string>(), npm}
                    : std::vector<std::string>{npm};
                command.insert(command.end(), {"ci", "--no-audit", "--no-fund",
                         spec.value("install_scripts", false) ? "--ignore-scripts=false" : "--ignore-scripts"});
                detail::execute_runtime_command(std::move(command), directory, env);
            } else throw std::runtime_error("environment kind must be python or node");
            if (spec.contains("source")) {
                fs::create_directories(directory / "source");
                unpack_archive(spec.at("source"), paths_, directory / "source");
            }
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
