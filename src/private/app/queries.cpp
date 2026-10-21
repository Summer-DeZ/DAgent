#include "app/queries.hpp"

#include <utility>
#include <charconv>
#include <sstream>
#include "base/text.hpp"
#include "base/path.hpp"
#include <algorithm>
#include "agent/permission.hpp"

#include "storage/history_read.hpp"
#include "workspace/context.hpp"
#include "workspace/files.hpp"

namespace dagent::app {
namespace {

/// @brief storage 错误 → backend 查询错误：backend 不包含 storage 头。
[[noreturn]] void rethrow(const storage::StorageError& error) {
    using Kind = backend::QueryError::Kind;
    const Kind kind = error.kind() == storage::StorageError::Kind::not_found ? Kind::not_found
                     : error.kind() == storage::StorageError::Kind::invalid_state ? Kind::invalid_state
                                                                                  : Kind::query_failed;
    throw backend::QueryError(kind, error.what());
}

/// @brief storage::HistoryRead → backend::HistoryReader（只读分页 + 释放语义）。
class HistoryReaderImpl final : public backend::HistoryReader {
public:
    HistoryReaderImpl(std::unique_ptr<storage::HistoryRead> read, std::string session_id)
        : read_(std::move(read)), session_id_(std::move(session_id)) {}

    backend::HistoryPage read(std::size_t limit) override {
        try {
            storage::HistoryRead::Page page = read_->read(limit);
            backend::HistoryPage out;
            out.items = std::move(page.items);
            out.done = page.done;
            return out;
        } catch (const storage::StorageError& error) {
            rethrow(error);
        }
    }
    const std::string& session_id() const override { return session_id_; }
    std::int64_t upper_seq() const override { return read_->upper_seq(); }

private:
    std::unique_ptr<storage::HistoryRead> read_;
    std::string session_id_;
};

} // namespace

QueryGatewayImpl::QueryGatewayImpl(storage::Options storage, std::filesystem::path cwd,
                                   std::filesystem::path project_root, workspace::SearchOptions search,
                                   std::shared_ptr<const agent::SkillCatalog> skills, exec::Options process,
                                   std::size_t completion_max_files, std::shared_ptr<const tools::MemoryStore> memory, std::vector<agent::SubagentDef> agents)
    : skills_(std::move(skills)), memory_(std::move(memory)), agents_(std::move(agents)), process_(std::move(process)), storage_(std::move(storage)), cwd_(std::move(cwd)), project_root_(std::move(project_root)),
      search_(std::move(search)), completion_max_files_(completion_max_files) {}

std::vector<agent::MemoryView> QueryGatewayImpl::memory_list() const {
    std::vector<agent::MemoryView> entries;
    for (const auto& entry : memory_->list())
        entries.push_back({"list", entry.scope, entry.name, entry.type, entry.updated, entry.description, {}});
    return entries;
}

agent::MemoryView QueryGatewayImpl::memory_view(std::string_view scope, std::string_view name) const {
    const auto entry = memory_->view(scope, name);
    return {"view", entry.scope, entry.name, entry.type, entry.updated, entry.description, entry.content};
}

agent::MemoryView QueryGatewayImpl::memory_delete(std::string_view scope, std::string_view name) {
    memory_->erase(scope, name);
    return {"delete", std::string(scope), std::string(name), {}, {}, {}, {}};
}

std::vector<backend::SessionSummary> QueryGatewayImpl::sessions(std::size_t limit) {
    std::vector<backend::SessionSummary> out;
    try {
        for (const storage::Summary& summary : storage::list(storage_, cwd_, limit)) {
            out.push_back({summary.meta.id, summary.title, summary.updated});
        }
    } catch (const storage::StorageError& error) {
        rethrow(error);
    }
    return out;
}

std::unique_ptr<backend::HistoryReader> QueryGatewayImpl::open_history(std::string_view session_id) {
    try {
        return std::make_unique<HistoryReaderImpl>(
            storage::HistoryRead::open(storage_, session_id), std::string(session_id));
    } catch (const storage::StorageError& error) {
        rethrow(error);
    }
}

std::vector<backend::ChildSummary> QueryGatewayImpl::children(std::string_view session_id) {
    std::vector<backend::ChildSummary> out;
    try {
        for (const storage::Summary& summary : storage::list_children(storage_, session_id)) {
            out.push_back({summary.meta.id, summary.meta.agent_name, summary.title});
        }
    } catch (const storage::StorageError& error) {
        rethrow(error);
    }
    return out;
}

backend::WorkspaceInfo QueryGatewayImpl::workspace() {
    backend::WorkspaceInfo info;
    info.cwd = cwd_;
    info.project_root = project_root_;
    workspace::ContextOptions options; options.process = process_; options.sandbox = search_.sandbox;
    const auto git = workspace::collect_git_status(cwd_, options);
    if (git) {
        info.branch = git->branch;
        if (git->dirty) info.branch += "*";
    }
    return info;
}

bool QueryGatewayImpl::should_watch_directory(const std::filesystem::path& path) const {
    const bool protected_path = std::ranges::any_of(search_.sandbox.protected_read, [&](const auto& root) {
        const auto relative = path.lexically_relative(root);
        return !relative.empty() && *relative.begin() != "..";
    });
    const auto relative = path.lexically_relative(project_root_).generic_string();
    return !(protected_path || (agent::classify_resource(path, true, {}) != agent::ResourceClass::normal &&
        relative != ".git" && !relative.starts_with(".git/refs") && !relative.starts_with(".git/logs")) ||
        relative == ".git/objects");
}

nlohmann::json QueryGatewayImpl::agents() const {
    auto result = nlohmann::json::array();
    for (const auto& def : agents_) result.push_back({{"name", def.name}, {"description", def.description},
        {"permission", def.permission}, {"tools", def.tools ? nlohmann::json(*def.tools) : nlohmann::json(nullptr)}});
    return result;
}

nlohmann::json QueryGatewayImpl::git_status() {
    using nlohmann::json;
    json result{{"repository", false}, {"branch", ""}, {"ahead", 0}, {"behind", 0}, {"files", json::array()}};
    const auto git = [&](std::vector<std::string> args) {
        exec::Command command;
        command.argv = {base::path_to_utf8(exec::system_git_program()), "--no-optional-locks", "-c", "core.quotepath=off", "-c", "core.fsmonitor=false",
                        "-c", "core.hooksPath=/dev/null", "-c", "core.pager=cat"};
        command.argv.insert(command.argv.end(), args.begin(), args.end());
        command.env_set = {{"GIT_CONFIG_NOSYSTEM", "1"}, {"GIT_CONFIG_GLOBAL", "/dev/null"}};
        command.cwd = project_root_; command.timeout = std::chrono::seconds(2);
        auto options = process_; options.max_output_bytes = 1 << 20;
        std::string output;
        auto result = search_.sandbox.run(command, options, [&](exec::Stream stream, std::string_view bytes) {
            if (stream == exec::Stream::out && output.size() < options.max_output_bytes)
                output.append(bytes.substr(0, options.max_output_bytes - output.size()));
        });
        const auto last = output.rfind('\0');
        output.resize(last == std::string::npos ? 0 : last + 1);
        result.out = std::move(output);
        return result;
    };
    const auto status = git({"status", "--porcelain=v2", "--branch", "-z", "--untracked-files=normal"});
    if (status.exit_code != 0) return result;
    result["repository"] = true;
    std::string oid;
    std::map<std::string, std::pair<int, int>> counts;
    const auto number = [](std::string_view text) {
        int n = 0; std::from_chars(text.data(), text.data() + text.size(), n); return n;
    };
    // NUL-delimited records preserve spaces, tabs and newlines in paths. Disable renames
    // in numstat so both old and new paths have ordinary records.
    for (const bool cached : {false, true}) {
        std::vector<std::string> args{"diff", "--numstat", "-z", "--no-renames", "--no-ext-diff", "--no-textconv"};
        if (cached) args.push_back("--cached");
        const auto diff = git(std::move(args));
        if (diff.exit_code != 0) continue;
        std::istringstream records(diff.out);
        for (std::string record; std::getline(records, record, '\0');) {
            const auto a = record.find('\t'), b = a == std::string::npos ? a : record.find('\t', a + 1);
            if (b == std::string::npos) continue;
            auto& count = counts[record.substr(b + 1)];
            count.first += number(std::string_view(record).substr(0, a));
            count.second += number(std::string_view(record).substr(a + 1, b - a - 1));
        }
    }
    std::istringstream records(status.out);
    for (std::string record; std::getline(records, record, '\0');) {
        if (record.starts_with("# branch.head ")) result["branch"] = base::to_valid_utf8(record.substr(14));
        else if (record.starts_with("# branch.oid ")) oid = record.substr(13, 7);
        else if (record.starts_with("# branch.ab ")) {
            std::istringstream ab(record.substr(12)); int ahead = 0, behind = 0; ab >> ahead >> behind;
            result["ahead"] = ahead; result["behind"] = -behind;
        } else if (record.size() > 2 && record[0] != '#') {
            std::size_t path_start = 2;
            std::string state = "?";
            if (record[0] != '?') {
                const int fields = record[0] == '1' ? 8 : record[0] == '2' ? 9 : 10;
                path_start = 0;
                for (int i = 0; i < fields && path_start != std::string::npos; ++i) {
                    path_start = record.find(' ', path_start);
                    if (path_start != std::string::npos) ++path_start;
                }
                if (path_start == std::string::npos) continue;
                const auto xy = record.substr(2, 2);
                state = xy.find('D') != std::string::npos ? "D" : xy.find('A') != std::string::npos ? "A" : "M";
            }
            const auto path = record.substr(path_start);
            const auto count = counts[path];
            if (result["files"].size() < 2048) result["files"].push_back({{"path", base::to_valid_utf8(path)},
                {"status", state}, {"added", count.first}, {"removed", count.second}});
            if (record[0] == '2') std::getline(records, record, '\0');
        }
    }
    if (result["branch"] == "(detached)") result["branch"] = oid;
    return result;
}

std::vector<backend::FileCandidate> QueryGatewayImpl::complete(std::string_view query, std::size_t limit) {
    workspace::FilesQuery query_options;
    query_options.root = project_root_;
    query_options.max_files = completion_max_files_;
    auto candidates = workspace::files(query_options, search_);
    std::erase_if(candidates, [&](const auto& candidate) {
        const auto path = project_root_ / candidate;
        if (agent::classify_resource(path, true, {}) != agent::ResourceClass::normal) return true;
        return std::ranges::any_of(search_.sandbox.protected_read, [&](const auto& root) {
            const auto relative = path.lexically_relative(root);
            return !relative.empty() && *relative.begin() != "..";
        });
    });
    std::vector<backend::FileCandidate> out;
    for (std::size_t index : workspace::fuzzy_rank(query, candidates, limit)) {
        out.push_back({candidates[index], false});
    }
    return out;
}

} // namespace dagent::app
