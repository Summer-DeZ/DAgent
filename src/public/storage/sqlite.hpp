/// @file sqlite.hpp
/// @brief storage 内部共用的 SQLite 语句与错误处理；数据库策略留在各自存储实现。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "storage/storage.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace dagent::storage::detail {

[[noreturn]] void fail(StorageError::Kind kind, const std::string& what);
void exec(sqlite3* db, std::string_view sql);

class Statement {
public:
    Statement(sqlite3* db, std::string_view sql);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    /// 保留 SQLite 的指针语义：value.data() 为空指针时绑定 NULL。
    void text(int index, std::string_view value);
    /// 空视图始终绑定空串，包括 data() 为空指针的视图。
    void text_or_empty(int index, std::string_view value);
    void integer(int index, std::int64_t value);
    void real(int index, double value);
    void blob(int index, std::string_view value);
    void null(int index);
    bool row();
    void done();
    std::string string(int column) const;
    std::string bytes(int column) const;
    std::int64_t integer(int column) const;
    /// 按 SQLite 规则转换为 double；业务所需的类型限制由调用方检查。
    double real(int column) const;
    int column_type(int column) const;
    bool is_null(int column) const;

private:
    void check(int rc);
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

} // namespace dagent::storage::detail
