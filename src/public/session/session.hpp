/// @file session.hpp
/// @brief 会话存储：事件只追加写入 JSONL，支持列出、回放、恢复；大内容转存 blob，敏感字段脱敏。
///
/// payload 对模块是不透明 JSON；事件里放什么由核心决定。
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

namespace dagent::session {

/// @brief 会话选项；数据库位置由安装根固定，只有记录与脱敏策略可配置。
struct Options {
    std::filesystem::path database;
    std::vector<std::string> redact_fields{"api_key", "authorization", "token"};
};

struct Meta {
    std::string id;
    std::filesystem::path cwd, git_root;
    std::string model;
    std::string created; ///< UTC ISO-8601，毫秒（展示/兼容字段）
};

struct Summary {
    Meta meta;
    std::string title;
    std::chrono::system_clock::time_point updated;
};

class SessionError : public std::runtime_error {
public:
    enum class Kind {
        io,       ///< 读写失败
        not_found,///< 会话不存在
        corrupt,  ///< 文件内容损坏
    };

    SessionError(Kind kind, const std::string& what) : std::runtime_error(what), kind_(kind) {}
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

/// @brief 按规范化 cwd 过滤并按更新时间倒序。
std::vector<Summary> list(const Options&, const std::filesystem::path& cwd, std::size_t limit);

/// @brief 按 seq 回放一个会话。
void replay(const Options&, std::string_view id,
            const std::function<void(std::string_view type, const nlohmann::json& payload)>& on_event);

/// @brief UUIDv7：前 48 位毫秒时间戳，按时间有序。
std::string new_id();

} // namespace dagent::session
