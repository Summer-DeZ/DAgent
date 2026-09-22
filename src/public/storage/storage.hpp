/// @file storage.hpp
/// @brief 会话存储：SQLite 只追加记录、脱敏、列表、回放、写租约与读写器打开入口。
///
/// payload 对模块是不透明 JSON；事件里放什么由核心决定。只读查询使用不初始化/修复数据库的连接。
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lib/nlohmann/json.hpp"
#include "agent/port_lease.hpp"
#include "agent/session_meta.hpp"

namespace dagent::agent {
class SessionStore;
} // namespace dagent::agent

namespace dagent::storage {

/// @brief 会话选项；数据库位置由安装根固定，只有记录与脱敏策略可配置。
struct Options {
    std::filesystem::path database;
    std::vector<std::string> redact_fields{"api_key", "authorization", "token"};
};

using Meta = agent::SessionMeta;

struct Summary {
    Meta meta;
    std::string title;
    std::chrono::system_clock::time_point updated;
};

class StorageError : public std::runtime_error {
public:
    enum class Kind {
        io,            ///< 读写失败
        not_found,     ///< 会话不存在
        corrupt,       ///< 文件内容损坏
        invalid_state, ///< 查询游标已释放或状态不允许
    };

    StorageError(Kind kind, const std::string& what) : std::runtime_error(what), kind_(kind) {}
    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

/// @brief SQLite 会话写入器；一轮事件在事务里提交。
class Writer {
public:
    static Writer create(const Options&, Meta meta);
    static Writer resume(const Options&, std::string_view id);

    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// @brief 脱敏后把 JSON payload 直接写入 events BLOB。
    void append(std::string_view type, nlohmann::json payload);

    /// @brief 刷盘；核心按自己的节奏调用（比如每轮结束），不是每行都 fsync。
    void sync();

    const Meta& meta() const noexcept;

private:
    struct Impl;
    explicit Writer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// @brief 同一 session_id 的跨进程可写所有权（安装根 `.runtime/session-locks/<id>.lock` 的 flock）。
///
/// 同一进程内按路径复用同一个 flock，供同会话切模型复用；跨进程冲突直接报 session_in_use。
/// FD 为 CLOEXEC；锁文件不 unlink。只读历史查询不取锁。
class SessionWriteLease : public agent::SessionLease {
public:
    /// @brief 取得或复用本进程已持有的同会话写租约；被其他进程持有时抛 StorageError(io)。
    static std::shared_ptr<SessionWriteLease> acquire(const Options&, std::string_view session_id);

    ~SessionWriteLease() override;
    SessionWriteLease(const SessionWriteLease&) = delete;
    SessionWriteLease& operator=(const SessionWriteLease&) = delete;

    const std::string& session_id() const noexcept override;

private:
    struct State;
    explicit SessionWriteLease(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

/// @brief 按规范化 cwd 过滤并按更新时间倒序；数据库不存在时返回空。
std::vector<Summary> list(const Options&, const std::filesystem::path& cwd, std::size_t limit);

/// @brief 按父会话列出子会话，按创建时间升序；供界面切换与按需回放使用。
std::vector<Summary> list_children(const Options&, std::string_view parent_id);

/// @brief 按 seq 回放一个会话；只读连接，不初始化或修复数据库。
void replay(const Options&, std::string_view id,
            const std::function<void(std::string_view type, const nlohmann::json& payload)>& on_event);

/// @brief 只读打开一个会话存储（查询与 Writer 打开入口；不建立连接）。
std::unique_ptr<agent::SessionStore> open_store(const Options&);

/// @brief UUIDv7：前 48 位毫秒时间戳，按时间有序。
std::string new_id();

} // namespace dagent::storage
