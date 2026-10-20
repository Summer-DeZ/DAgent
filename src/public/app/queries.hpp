/// @file queries.hpp
/// @brief 装配侧查询与记忆管理网关：会话列表、历史投影、项目信息与文件补全。
///
/// 实现 backend::QueryGateway；在 backend 的查询线程调用，不初始化/修复数据库，
/// 不借用执行线程的 Writer。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <thread>

#include "backend/queries.hpp"
#include "agent/subagent_def.hpp"
#include "tools/memory.hpp"
#include "exec/process.hpp"
#include "storage/storage.hpp"
#include "workspace/search.hpp"

namespace dagent::app {

class QueryGatewayImpl final : public backend::QueryGateway {
public:
    QueryGatewayImpl(storage::Options storage, std::filesystem::path cwd,
                     std::filesystem::path project_root, workspace::SearchOptions search,
                     std::shared_ptr<const agent::SkillCatalog> skills, exec::Options process,
                     std::size_t completion_max_files, std::shared_ptr<const tools::MemoryStore> memory, std::vector<agent::SubagentDef> agents);

    std::vector<backend::SessionSummary> sessions(std::size_t limit) override;
    std::unique_ptr<backend::HistoryReader> open_history(std::string_view session_id) override;
    std::vector<backend::ChildSummary> children(std::string_view session_id) override;
    backend::WorkspaceInfo workspace() override;
    nlohmann::json git_status() override;
    void watch_workspace(std::function<void()> changed) override;
    nlohmann::json agents() const override;
    const agent::SkillCatalog& skills() const override { return *skills_; }
    std::vector<agent::MemoryView> memory_list() const override;
    agent::MemoryView memory_view(std::string_view scope, std::string_view name) const override;
    agent::MemoryView memory_delete(std::string_view scope, std::string_view name) override;
    std::vector<backend::FileCandidate> complete(std::string_view query, std::size_t limit) override;

private:
    std::shared_ptr<const agent::SkillCatalog> skills_;
    std::shared_ptr<const tools::MemoryStore> memory_;
    std::vector<agent::SubagentDef> agents_;
    exec::Options process_;
    storage::Options storage_;
    std::filesystem::path cwd_, project_root_;
    workspace::SearchOptions search_;
    std::size_t completion_max_files_;
    std::jthread watcher_;
};

} // namespace dagent::app
