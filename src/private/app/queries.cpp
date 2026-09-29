#include "app/queries.hpp"

#include <utility>
#include <algorithm>
#include "agent/permission.hpp"

#include "storage/history_read.hpp"
#include "workspace/context.hpp"
#include "workspace/files.hpp"

namespace dagent::app {
namespace {

/// @brief storage 错误 → runtime 中立查询错误：backend 不包含 storage 头。
[[noreturn]] void rethrow(const storage::StorageError& error) {
    using Kind = runtime::QueryError::Kind;
    const Kind kind = error.kind() == storage::StorageError::Kind::not_found ? Kind::not_found
                     : error.kind() == storage::StorageError::Kind::invalid_state ? Kind::invalid_state
                                                                                  : Kind::query_failed;
    throw runtime::QueryError(kind, error.what());
}

/// @brief storage::HistoryRead → runtime::HistoryReader（只读分页 + 释放语义）。
class HistoryReaderImpl final : public runtime::HistoryReader {
public:
    HistoryReaderImpl(std::unique_ptr<storage::HistoryRead> read, std::string session_id)
        : read_(std::move(read)), session_id_(std::move(session_id)) {}

    runtime::HistoryPage read(std::size_t limit) override {
        try {
            storage::HistoryRead::Page page = read_->read(limit);
            runtime::HistoryPage out;
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
                                   std::size_t completion_max_files)
    : skills_(std::move(skills)), process_(std::move(process)), storage_(std::move(storage)), cwd_(std::move(cwd)), project_root_(std::move(project_root)),
      search_(std::move(search)), completion_max_files_(completion_max_files) {}

std::vector<runtime::SessionSummary> QueryGatewayImpl::sessions(std::size_t limit) {
    std::vector<runtime::SessionSummary> out;
    try {
        for (const storage::Summary& summary : storage::list(storage_, cwd_, limit)) {
            out.push_back({summary.meta.id, summary.title, summary.updated});
        }
    } catch (const storage::StorageError& error) {
        rethrow(error);
    }
    return out;
}

std::unique_ptr<runtime::HistoryReader> QueryGatewayImpl::open_history(std::string_view session_id) {
    try {
        return std::make_unique<HistoryReaderImpl>(
            storage::HistoryRead::open(storage_, session_id), std::string(session_id));
    } catch (const storage::StorageError& error) {
        rethrow(error);
    }
}

std::vector<runtime::ChildSummary> QueryGatewayImpl::children(std::string_view session_id) {
    std::vector<runtime::ChildSummary> out;
    try {
        for (const storage::Summary& summary : storage::list_children(storage_, session_id)) {
            out.push_back({summary.meta.id, summary.meta.agent_name, summary.title});
        }
    } catch (const storage::StorageError& error) {
        rethrow(error);
    }
    return out;
}

runtime::WorkspaceInfo QueryGatewayImpl::workspace() {
    runtime::WorkspaceInfo info;
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

std::vector<runtime::FileCandidate> QueryGatewayImpl::complete(std::string_view query, std::size_t limit) {
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
    std::vector<runtime::FileCandidate> out;
    for (std::size_t index : workspace::fuzzy_rank(query, candidates, limit)) {
        out.push_back({candidates[index], false});
    }
    return out;
}

} // namespace dagent::app
