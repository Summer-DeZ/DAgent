/// @file queries.hpp
/// @brief 装配侧只读查询网关：会话列表、历史投影、项目信息与文件补全。
///
/// 实现 runtime::QueryGateway；在 runtime 的查询线程调用，不初始化/修复数据库，
/// 不借用执行线程的 Writer。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/factory.hpp"
#include "exec/process.hpp"
#include "storage/storage.hpp"
#include "workspace/search.hpp"

namespace dagent::app {

class QueryGatewayImpl final : public runtime::QueryGateway {
public:
    QueryGatewayImpl(storage::Options storage, std::filesystem::path cwd,
                     std::filesystem::path project_root, workspace::SearchOptions search,
                     std::shared_ptr<const agent::SkillCatalog> skills, exec::Options process);

    std::vector<runtime::SessionSummary> sessions(std::size_t limit) override;
    std::unique_ptr<runtime::HistoryReader> open_history(std::string_view session_id) override;
    std::vector<runtime::ChildSummary> children(std::string_view session_id) override;
    runtime::WorkspaceInfo workspace() override;
    const agent::SkillCatalog& skills() const override { return *skills_; }
    std::vector<runtime::FileCandidate> complete(std::string_view query, std::size_t limit) override;

private:
    std::shared_ptr<const agent::SkillCatalog> skills_;
    exec::Options process_;
    storage::Options storage_;
    std::filesystem::path cwd_, project_root_;
    workspace::SearchOptions search_;
    std::vector<std::string> file_cache_;
};

} // namespace dagent::app
