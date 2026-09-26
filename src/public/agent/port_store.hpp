/// @file port_store.hpp
/// @brief SessionStore 端口：会话元信息、记录读取与写入器的打开入口。
///
/// 实现在 storage 模块。只读查询使用独立连接，不借用执行线程的 Writer；
/// Writer 与写租约以独占所有权返回。
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/port_journal.hpp"

namespace dagent::agent {

/// @brief 一条从存储读出的记录：seq、类型与未解释的 payload（RecordCodec 在核心）。
struct StoredRecord {
    std::int64_t seq = 0;
    std::string type;
    nlohmann::json payload;
};

/// @brief 会话存储的读取/打开能力。错误以 RecordError 抛出。
class SessionStore {
public:
    virtual ~SessionStore() = default;

    /// @brief 按 seq 升序读取一个会话 [from_seq, to_seq) 的记录；高水位分页由调用方推进。
    virtual std::vector<StoredRecord> read_records(std::string_view session_id, std::int64_t from_seq,
                                                   std::int64_t to_seq) = 0;

    /// @brief 会话当前的最大 seq（高水位）；会话不存在抛 not_found。
    virtual std::int64_t max_seq(std::string_view session_id) = 0;

    /// @brief 打开一个追加写入器；实现决定 create/resume 语义与事务边界。
    virtual std::unique_ptr<JournalWriter> open_writer_create(const SessionMeta&) = 0;
    virtual std::unique_ptr<JournalWriter> open_writer_resume(std::string_view session_id) = 0;
};

} // namespace dagent::agent
