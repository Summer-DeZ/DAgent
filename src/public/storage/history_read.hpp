/// @file history_read.hpp
/// @brief 只读历史分页查询：固定高水位、跨页验证游标与显示投影。
///
/// 由 session.history 打开；不初始化/修复数据库、不取写锁、不构造 Session/模型/MCP。
/// 每页最多扫描 100 条记录，游标由后端原样回传；读完/关闭/连接关闭时释放。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "agent/history.hpp"
#include "storage/storage.hpp"

namespace dagent::storage {

class HistoryRead {
public:
    struct Page {
        std::vector<agent::HistoryItem> items;
        std::string cursor; ///< 下一页游标；空表示已读完
        bool done = false;
    };

    /// @brief 捕获 Meta 与 MAX(seq) 高水位；会话不存在抛 not_found，库不可读抛 io。
    static std::unique_ptr<HistoryRead> open(const Options&, std::string_view session_id);

    ~HistoryRead();
    HistoryRead(const HistoryRead&) = delete;
    HistoryRead& operator=(const HistoryRead&) = delete;

    /// @brief 读取一页；cursor 为空表示第一页。已释放查询抛 invalid_state。
    Page read(const std::string& cursor, std::size_t limit);
    /// @brief 前端关闭/切换页面时释放未读完的查询；幂等。
    void close();

    const Meta& meta() const;
    std::int64_t upper_seq() const;

private:
    struct Impl;
    explicit HistoryRead(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace dagent::storage
