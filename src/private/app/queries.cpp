#include "app/queries.hpp"

#include <utility>

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
    explicit HistoryReaderImpl(std::unique_ptr<storage::HistoryRead> read) : read_(std::move(read)) {}

    runtime::HistoryPage read(const std::string& cursor, std::size_t limit) override {
        try {
            storage::HistoryRead::Page page = read_->read(cursor, limit);
            runtime::HistoryPage out;
            out.items = std::move(page.items);
            out.cursor = std::move(page.cursor);
            out.done = page.done;
            return out;
        } catch (const storage::StorageError& error) {
            rethrow(error);
        }
    }
    const std::string& session_id() const override { return session_id_; }
    std::int64_t upper_seq() const override { return read_->upper_seq(); }

    void bind_session(std::string id) { session_id_ = std::move(id); }

private:
    std::unique_ptr<storage::HistoryRead> read_;
    std::string session_id_;
};

} // namespace

QueryGatewayImpl::QueryGatewayImpl(storage::Options storage, std::filesystem::path cwd,
                                   std::filesystem::path project_root, workspace::SearchOptions search)
    : storage_(std::move(storage)), cwd_(std::move(cwd)), project_root_(std::move(project_root)),
      search_(std::move(search)) {}

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
        auto reader =
            std::make_unique<HistoryReaderImpl>(storage::HistoryRead::open(storage_, session_id));
        reader->bind_session(std::string(session_id));
        return reader;
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
    const workspace::Environment env = workspace::collect_environment(cwd_);
    if (env.git) {
        info.branch = env.git->branch;
        if (!env.git->status_summary.empty()) info.branch += "*";
    }
    return info;
}

std::vector<runtime::FileCandidate> QueryGatewayImpl::complete(std::string_view query, std::size_t limit) {
    if (file_cache_.empty()) {
        workspace::FilesQuery files;
        files.root = project_root_;
        files.max_files = 5000;
        file_cache_ = workspace::files(files, search_);
    }
    std::vector<runtime::FileCandidate> out;
    for (std::size_t index : workspace::fuzzy_rank(query, file_cache_, limit)) {
        out.push_back({file_cache_[index], false});
    }
    return out;
}

} // namespace dagent::app
