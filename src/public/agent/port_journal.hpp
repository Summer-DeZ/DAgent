/// @file port_journal.hpp
/// @brief JournalWriter 端口与记录错误：持久历史的唯一写入口。
///
/// 实现在 storage 模块；失败抛 RecordError，类别固定 io/not_found/corrupt，
/// 只在核心提交/恢复边界转换（record-routes §4.3）。
#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lib/nlohmann/json.hpp"
#include "agent/session_meta.hpp"

namespace dagent::agent {

/// @brief 记录读写的错误分类；与既有会话存储错误类别一致。
class RecordError : public std::runtime_error {
public:
    enum class Kind { io, not_found, corrupt };

    RecordError(Kind kind, const std::string& what) : std::runtime_error(what), kind_(kind) {}
    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

/// @brief 一条已编码记录：既有 events.type + payload 形状（记录路线 §2）。
struct Record {
    std::string_view type;
    nlohmann::json payload;
};

/// @brief 追加式记录写入器；一个会话一个实例，独占所有权。
class JournalWriter {
public:
    virtual ~JournalWriter() = default;

    /// @brief 脱敏与编码由上游（RecordCodec）完成后写入；失败抛 RecordError。
    virtual void append(const Record&) = 0;

    /// @brief 刷盘；调用节奏由提交器决定（不是每条 fsync）。失败抛 RecordError。
    virtual void sync() = 0;

    /// @brief 会话元信息（id/父关系/创建时间）；由打开它的存储提供。
    virtual const SessionMeta& meta() const = 0;
};

} // namespace dagent::agent
