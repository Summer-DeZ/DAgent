/// @file queries.hpp
/// @brief backend 的查询端口、DTO 与错误分类，由 app 适配具体存储和工作区实现。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agent/history.hpp"
#include "agent/skills.hpp"
#include "agent/tool_data.hpp"
#include "lib/nlohmann/json.hpp"

namespace dagent::backend {

struct SessionSummary {
    std::string id;
    std::string title;
    std::chrono::system_clock::time_point updated;
};

struct WorkspaceInfo {
    std::filesystem::path cwd;
    std::filesystem::path project_root;
    std::string branch;
};

struct FileCandidate {
    std::string path;
    bool directory = false;
};

struct ChildSummary {
    std::string session_id, agent, title;
};

/// @brief 一页顺序读取的历史；查询游标由 backend 独立管理。
struct HistoryPage {
    std::vector<agent::HistoryItem> items;
    bool done = false;
};

/// @brief 只读查询失败的中立分类：app 适配层把存储/IO 错误翻译到这里，backend 只按 kind 映射协议错误。
class QueryError : public std::runtime_error {
public:
    enum class Kind { not_found, invalid_state, query_failed };

    QueryError(Kind kind, std::string message)
        : std::runtime_error(std::move(message)), kind_(kind) {}
    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

/// @brief 一个打开的历史查询：固定高水位、顺序读取；释放后读取返回错误。
class HistoryReader {
public:
    virtual ~HistoryReader() = default;

    virtual HistoryPage read(std::size_t limit) = 0;
    virtual const std::string& session_id() const = 0;
    virtual std::int64_t upper_seq() const = 0;
};

/// @brief 查询与记忆管理：在查询线程调用，不碰当前会话 Writer。
class QueryGateway {
public:
    virtual ~QueryGateway() = default;

    virtual std::vector<SessionSummary> sessions(std::size_t limit) = 0;
    virtual std::unique_ptr<HistoryReader> open_history(std::string_view session_id) = 0;
    virtual std::vector<ChildSummary> children(std::string_view session_id) = 0;
    virtual WorkspaceInfo workspace() = 0;
    virtual nlohmann::json git_status() = 0;
    virtual void watch_workspace(std::function<void()> changed) = 0;
    virtual nlohmann::json agents() const = 0;
    virtual const agent::SkillCatalog& skills() const = 0;
    virtual std::vector<agent::MemoryView> memory_list() const = 0;
    virtual agent::MemoryView memory_view(std::string_view scope, std::string_view name) const = 0;
    virtual agent::MemoryView memory_delete(std::string_view scope, std::string_view name) = 0;
    virtual std::vector<FileCandidate> complete(std::string_view query, std::size_t limit) = 0;
};

} // namespace dagent::backend
