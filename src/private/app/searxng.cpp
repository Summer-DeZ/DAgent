#include "app/searxng.hpp"

#include <condition_variable>
#include <fstream>
#include <mutex>
#include <openssl/rand.h>
#include <unistd.h>
#include "exec/child.hpp"
#include "lib/nlohmann/json.hpp"
#include "net/http.hpp"

namespace dagent::app {
namespace fs = std::filesystem;

struct Searxng::Impl {
    HomePaths paths;
    fs::path environment, directory;
    web::Options options;
    exec::Options process;
    std::mutex mutex;
    struct State {
        std::mutex mutex;
        std::condition_variable_any cv;
        int port = 0;
        bool exited = false;
        std::string error;
    };
    std::shared_ptr<State> state;
    std::unique_ptr<exec::Child> child;

    ~Impl() {
        child.reset();
        std::error_code ec;
        if (!directory.empty()) fs::remove_all(directory, ec);
    }

    std::string start(std::stop_token stop) {
        const std::lock_guard lock(mutex);
        if (stop.stop_requested()) throw exec::ExecError(exec::ExecError::Kind::cancelled, "search startup cancelled");
        if (state) {
            const std::lock_guard state_lock(state->mutex);
            if (!state->exited && state->port) return "http://127.0.0.1:" + std::to_string(state->port);
        }
        child.reset();
        if (environment.empty() || !fs::exists(environment / "source/searx/webapp.py"))
            throw std::runtime_error("SearXNG is not prepared; run dagent runtime sync");
        if (directory.empty()) {
            fs::create_directories(paths.run);
            auto pattern = (paths.run / "searxng-XXXXXX").string();
            if (!::mkdtemp(pattern.data())) throw std::runtime_error("cannot create SearXNG run directory");
            directory = pattern;
        }
        unsigned char random[32];
        if (RAND_bytes(random, sizeof(random)) != 1) throw std::runtime_error("cannot generate SearXNG secret");
        constexpr char hex[] = "0123456789abcdef";
        std::string secret;
        for (auto c : random) { secret += hex[c >> 4]; secret += hex[c & 15]; }
        // JSON is a YAML subset; engine names cannot inject settings.
        nlohmann::json settings{
            {"use_default_settings", {{"engines", {{"keep_only", options.engines}}}}},
            {"general", {{"debug", false}, {"enable_metrics", false}}},
            {"server", {{"secret_key", secret}, {"bind_address", "127.0.0.1"}, {"port", 0}, {"limiter", false}}},
            {"search", {{"formats", {"json"}}}},
            {"outgoing", {{"request_timeout", 8.0}}},
            {"engines", nlohmann::json::array()}};
        for (const auto& name : options.engines) settings["engines"].push_back({{"name", name}, {"disabled", false}});
        const auto config = directory / "settings.yml";
        std::ofstream output(config);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output << settings.dump(2); output.close();
        fs::permissions(config, fs::perms::owner_read | fs::perms::owner_write);
        const auto executable = fs::read_symlink("/proc/self/exe").parent_path();
        exec::Command command;
        command.argv = {(environment / "bin/python").string(), "-u", (executable / "libexec/searxng_server.py").string()};
        command.cwd = directory;
        command.inherit_env = false;
        command.env_set = {{"PYTHONPATH", (environment / "source").string()},
            {"SEARXNG_SETTINGS_PATH", config.string()}, {"PYTHONDONTWRITEBYTECODE", "1"},
            {"PYTHONNOUSERSITE", "1"}, {"HOME", directory.string()}, {"LC_ALL", "C.UTF-8"}};
        // Infrastructure follows the backend's proxy configuration, just like model API connections.
        for (const auto* name : {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY", "http_proxy", "https_proxy", "all_proxy", "no_proxy"})
            if (const char* value = std::getenv(name)) command.env_set.emplace_back(name, value);
        auto current = std::make_shared<State>();
        state = current;
        child = exec::Child::spawn(command, process);
        child->on_line([current](std::string_view line) {
            constexpr std::string_view prefix = "DAGENT_SEARXNG_PORT=";
            if (!line.starts_with(prefix)) return;
            const auto value = nlohmann::json::parse(line.substr(prefix.size()), nullptr, false);
            if (!value.is_number_integer()) return;
            const std::lock_guard lock(current->mutex);
            current->port = value.get<int>(); current->cv.notify_all();
        });
        child->on_stderr([current](std::string_view line) {
            const std::lock_guard lock(current->mutex);
            current->error = std::string(line.substr(0, 2048));
        });
        child->on_exit([current](auto, auto) {
            const std::lock_guard lock(current->mutex);
            current->exited = true; current->cv.notify_all();
        });
        std::unique_lock state_lock(current->mutex);
        const bool ready = current->cv.wait_for(state_lock, stop, options.startup_timeout,
            [&] { return current->port != 0 || current->exited; });
        const int port = current->port;
        const auto error = current->error;
        const bool failed = !ready || current->exited || !port;
        state_lock.unlock();
        if (failed) {
            child.reset();
            if (stop.stop_requested()) throw exec::ExecError(exec::ExecError::Kind::cancelled, "search startup cancelled");
            throw std::runtime_error("SearXNG did not become ready: " + error);
        }
        const auto endpoint = "http://127.0.0.1:" + std::to_string(port);
        net::HttpOptions health_options;
        health_options.timeout = std::chrono::seconds(3);
        health_options.max_body_bytes = 1024;
        try {
            net::HttpClient health(health_options);
            if (health.send({"GET", endpoint + "/healthz", {}, {}}, stop).status != 200)
                throw std::runtime_error("SearXNG health check failed");
        } catch (...) { child.reset(); throw; }
        return endpoint;
    }
};

Searxng::Searxng(HomePaths paths, fs::path environment, web::Options options, exec::Options process)
    : impl_(new Impl{std::move(paths), std::move(environment), {}, std::move(options), std::move(process), {}, {}, {}}) {}
Searxng::~Searxng() = default;
std::string Searxng::endpoint(std::stop_token stop) { return impl_->start(stop); }

} // namespace dagent::app
