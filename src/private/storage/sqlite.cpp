#include "storage/sqlite.hpp"

#include "lib/sqlite/sqlite3.h"

namespace dagent::storage::detail {

[[noreturn]] void fail(StorageError::Kind kind, const std::string& what) {
    throw StorageError(kind, what);
}

void exec(sqlite3* db, std::string_view sql) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, std::string(sql).c_str(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        const std::string text = message == nullptr ? sqlite3_errmsg(db) : message;
        sqlite3_free(message);
        fail(StorageError::Kind::io, text);
    }
}

Statement::Statement(sqlite3* db, std::string_view sql) : db_(db) {
    const int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
    if (rc != SQLITE_OK) fail(StorageError::Kind::io, sqlite3_errmsg(db_));
}

Statement::~Statement() { sqlite3_finalize(stmt_); }

void Statement::text(int index, std::string_view value) {
    check(sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
}

void Statement::text_or_empty(int index, std::string_view value) {
    static const std::string empty;
    text(index, value.empty() ? std::string_view(empty) : value);
}

void Statement::integer(int index, std::int64_t value) {
    check(sqlite3_bind_int64(stmt_, index, value));
}

void Statement::real(int index, double value) {
    check(sqlite3_bind_double(stmt_, index, value));
}

void Statement::blob(int index, std::string_view value) {
    check(sqlite3_bind_blob(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
}

void Statement::null(int index) { check(sqlite3_bind_null(stmt_, index)); }

bool Statement::row() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    check(rc);
    return false;
}

void Statement::done() {
    if (row()) fail(StorageError::Kind::corrupt, "unexpected row from SQLite statement");
}

std::string Statement::string(int column) const {
    const auto* value = sqlite3_column_text(stmt_, column);
    const int size = sqlite3_column_bytes(stmt_, column);
    return value == nullptr ? std::string{} : std::string(reinterpret_cast<const char*>(value), size);
}

std::string Statement::bytes(int column) const {
    const auto* value = sqlite3_column_blob(stmt_, column);
    const int size = sqlite3_column_bytes(stmt_, column);
    return value == nullptr ? std::string{} : std::string(static_cast<const char*>(value), size);
}

std::int64_t Statement::integer(int column) const { return sqlite3_column_int64(stmt_, column); }

double Statement::real(int column) const { return sqlite3_column_double(stmt_, column); }

int Statement::column_type(int column) const { return sqlite3_column_type(stmt_, column); }

bool Statement::is_null(int column) const { return column_type(column) == SQLITE_NULL; }

void Statement::check(int rc) {
    if (rc != SQLITE_OK) fail(StorageError::Kind::io, sqlite3_errmsg(db_));
}

} // namespace dagent::storage::detail
