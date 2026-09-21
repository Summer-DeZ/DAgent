#include "session/session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <random>
#include <string>
#include <utility>

#include "base/json.hpp"
#include "base/log.hpp"
#include "base/text.hpp"
#include "lib/sqlite/sqlite3.h"

namespace dagent::session {
namespace fs = std::filesystem;
namespace {

using nlohmann::json;

[[noreturn]] void fail(SessionError::Kind kind, const std::string& what) {
    throw SessionError(kind, what);
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string iso_from_ms(std::int64_t value) {
    const auto point = std::chrono::system_clock::time_point{std::chrono::milliseconds{value}};
    const std::time_t raw = std::chrono::system_clock::to_time_t(point);
    std::tm utc{};
    ::gmtime_r(&raw, &utc);
    const auto millis = value % 1000;
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
                       utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                       utc.tm_hour, utc.tm_min, utc.tm_sec, millis < 0 ? -millis : millis);
}

fs::path normalized(const fs::path& value) {
    std::error_code ec;
    fs::path path = fs::weakly_canonical(value, ec);
    if (!ec) return path;
    ec.clear();
    path = fs::absolute(value, ec);
    return ec ? value.lexically_normal() : path.lexically_normal();
}

std::string title_from(const json& payload) {
    const auto it = payload.find("text");
    if (it == payload.end() || !it->is_string()) return {};
    std::string title = base::to_valid_utf8(it->get<std::string>());
    if (const auto newline = title.find_first_of("\r\n"); newline != std::string::npos) title.resize(newline);
    std::size_t pos = 0;
    for (int count = 0; pos < title.size() && count < 60; ++count) {
        const auto lead = static_cast<unsigned char>(title[pos]);
        pos += lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    }
    title.resize(std::min(pos, title.size()));
    return title;
}

class Statement {
public:
    Statement(sqlite3* db, std::string_view sql) : db_(db) {
        const int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
        if (rc != SQLITE_OK) fail(SessionError::Kind::io, sqlite3_errmsg(db_));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void text(int index, std::string_view value) {
        check(sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }
    void integer(int index, std::int64_t value) { check(sqlite3_bind_int64(stmt_, index, value)); }
    void blob(int index, std::string_view value) {
        check(sqlite3_bind_blob(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }
    void null(int index) { check(sqlite3_bind_null(stmt_, index)); }
    bool row() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        check(rc);
        return false;
    }
    void done() {
        if (row()) fail(SessionError::Kind::corrupt, "unexpected row from SQLite statement");
    }
    std::string string(int column) const {
        const auto* value = sqlite3_column_text(stmt_, column);
        const int size = sqlite3_column_bytes(stmt_, column);
        return value == nullptr ? std::string{} : std::string(reinterpret_cast<const char*>(value), size);
    }
    std::string bytes(int column) const {
        const auto* value = sqlite3_column_blob(stmt_, column);
        const int size = sqlite3_column_bytes(stmt_, column);
        return value == nullptr ? std::string{} : std::string(static_cast<const char*>(value), size);
    }
    std::int64_t integer(int column) const { return sqlite3_column_int64(stmt_, column); }
    bool is_null(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

private:
    void check(int rc) {
        if (rc != SQLITE_OK) fail(SessionError::Kind::io, sqlite3_errmsg(db_));
    }
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

class Database {
public:
    explicit Database(const fs::path& file) : file_(file) {
        if (file_.empty()) fail(SessionError::Kind::io, "session database path is empty");
        open();
        try {
            initialize();
        } catch (const SessionError& error) {
            sqlite3_close(db_);
            db_ = nullptr;
            const fs::path backup = file_.string() + std::format(".corrupt-{}", now_ms());
            std::error_code ec;
            fs::rename(file_, backup, ec);
            if (ec) throw;
            base::logger("session")->error("session database was unreadable; moved it to {} and created a new database: {}",
                                           backup.string(), error.what());
            open();
            initialize();
        }
    }
    ~Database() { if (db_ != nullptr) sqlite3_close(db_); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    sqlite3* get() const { return db_; }
    void exec(std::string_view sql) {
        char* message = nullptr;
        const int rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &message);
        if (rc != SQLITE_OK) {
            const std::string text = message == nullptr ? sqlite3_errmsg(db_) : message;
            sqlite3_free(message);
            fail(SessionError::Kind::io, text);
        }
    }

private:
    void open() {
        const int rc = sqlite3_open_v2(file_.c_str(), &db_,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                       nullptr);
        if (rc != SQLITE_OK) {
            const std::string text = db_ == nullptr ? "cannot open SQLite database" : sqlite3_errmsg(db_);
            if (db_ != nullptr) sqlite3_close(db_);
            db_ = nullptr;
            fail(SessionError::Kind::io, text);
        }
        sqlite3_busy_timeout(db_, 5000);
    }
    int user_version() {
        Statement query(db_, "PRAGMA user_version");
        return query.row() ? static_cast<int>(query.integer(0)) : 0;
    }
    bool has_column(std::string_view table, std::string_view name) {
        Statement query(db_, "SELECT 1 FROM pragma_table_info(?) WHERE name=?");
        query.text(1, table);
        query.text(2, name);
        return query.row();
    }
    void initialize() {
        exec("PRAGMA foreign_keys=ON;");
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=NORMAL;");
        exec(R"SQL(
CREATE TABLE IF NOT EXISTS sessions (
  id TEXT PRIMARY KEY,
  cwd TEXT NOT NULL,
  model TEXT NOT NULL,
  title TEXT,
  created INTEGER NOT NULL,
  updated INTEGER NOT NULL,
  open_turn INTEGER NOT NULL DEFAULT 0,
  parent_id TEXT,
  agent_name TEXT
);
CREATE INDEX IF NOT EXISTS sessions_by_cwd ON sessions(cwd, updated DESC);
CREATE TABLE IF NOT EXISTS events (
  session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
  seq INTEGER NOT NULL,
  type TEXT NOT NULL,
  payload BLOB NOT NULL,
  PRIMARY KEY (session_id, seq)
) WITHOUT ROWID;
)SQL");
        // 该库的迁移机制：每次 schema 变更递增 user_version，旧库就地补齐。
        if (user_version() < 1) {
            if (!has_column("sessions", "parent_id")) exec("ALTER TABLE sessions ADD COLUMN parent_id TEXT;");
            if (!has_column("sessions", "agent_name")) exec("ALTER TABLE sessions ADD COLUMN agent_name TEXT;");
            exec("CREATE INDEX IF NOT EXISTS sessions_by_parent ON sessions(parent_id, created);");
            exec("PRAGMA user_version=1;");
        }
    }
    fs::path file_;
    sqlite3* db_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE;"); }
    ~Transaction() {
        if (!committed_) {
            try { db_.exec("ROLLBACK;"); } catch (...) {}
        }
    }
    void commit() { db_.exec("COMMIT;"); committed_ = true; }
private:
    Database& db_;
    bool committed_ = false;
};

Meta read_meta(Database& db, std::string_view id, bool* open_turn = nullptr) {
    Statement query(db.get(),
                    "SELECT cwd,model,created,open_turn,parent_id,agent_name FROM sessions WHERE id=?");
    query.text(1, id);
    if (!query.row()) fail(SessionError::Kind::not_found, "session not found: " + std::string(id));
    Meta meta;
    meta.id = std::string(id);
    meta.cwd = query.string(0);
    meta.model = query.string(1);
    meta.created = iso_from_ms(query.integer(2));
    if (open_turn != nullptr) *open_turn = query.integer(3) != 0;
    if (!query.is_null(4)) meta.parent_id = query.string(4);
    if (!query.is_null(5)) meta.agent_name = query.string(5);
    return meta;
}

} // namespace

std::string new_id() {
    static thread_local std::mt19937_64 rng([] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }());
    const auto milliseconds = static_cast<std::uint64_t>(now_ms());
    const std::uint64_t rand_a = rng() & 0xFFF;
    const std::uint64_t rand_b = rng() & ((1ULL << 62) - 1);
    std::uint8_t bytes[16]{};
    for (int i = 0; i < 6; ++i) bytes[i] = static_cast<std::uint8_t>(milliseconds >> (40 - 8 * i));
    bytes[6] = static_cast<std::uint8_t>(0x70 | ((rand_a >> 8) & 0x0F));
    bytes[7] = static_cast<std::uint8_t>(rand_a);
    bytes[8] = static_cast<std::uint8_t>(0x80 | ((rand_b >> 56) & 0x3F));
    for (int i = 0; i < 7; ++i) bytes[9 + i] = static_cast<std::uint8_t>(rand_b >> (48 - 8 * i));
    constexpr std::string_view hex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += hex[bytes[i] >> 4];
        out += hex[bytes[i] & 0x0F];
    }
    return out;
}

struct Writer::Impl {
    Options options;
    Meta meta;
    Database db;
    std::int64_t seq = 0;

    Impl(Options value, Meta session)
        : options(std::move(value)), meta(std::move(session)), db(options.database) {}
};

Writer::Writer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&&) noexcept = default;
Writer::~Writer() = default;

Writer Writer::create(const Options& options, Meta meta) {
    if (meta.id.empty()) meta.id = new_id();
    meta.cwd = normalized(meta.cwd);
    const auto timestamp = now_ms();
    meta.created = iso_from_ms(timestamp);
    auto impl = std::make_unique<Impl>(options, std::move(meta));
    Statement insert(impl->db.get(),
                     "INSERT INTO sessions(id,cwd,model,title,created,updated,open_turn,parent_id,agent_name) "
                     "VALUES(?,?,?,NULL,?,?,0,?,?)");
    insert.text(1, impl->meta.id);
    insert.text(2, impl->meta.cwd.string());
    insert.text(3, impl->meta.model);
    insert.integer(4, timestamp);
    insert.integer(5, timestamp);
    if (impl->meta.parent_id.empty()) insert.null(6); else insert.text(6, impl->meta.parent_id);
    if (impl->meta.agent_name.empty()) insert.null(7); else insert.text(7, impl->meta.agent_name);
    insert.done();
    return Writer(std::move(impl));
}

Writer Writer::resume(const Options& options, std::string_view id) {
    Database db(options.database);
    Meta meta = read_meta(db, id);
    auto impl = std::make_unique<Impl>(options, std::move(meta));
    Statement sequence(impl->db.get(), "SELECT COALESCE(MAX(seq),-1)+1 FROM events WHERE session_id=?");
    sequence.text(1, id);
    if (sequence.row()) impl->seq = sequence.integer(0);
    return Writer(std::move(impl));
}

void Writer::append(std::string_view type, json payload) {
    base::redact(payload, impl_->options.redact_fields);
    const std::string encoded = payload.dump();
    const auto timestamp = now_ms();
    const bool begins = type == "user";
    const bool ends = type == "turn_end";
    std::optional<Transaction> tx;
    if (begins || ends) tx.emplace(impl_->db);

    Statement event(impl_->db.get(), "INSERT INTO events(session_id,seq,type,payload) VALUES(?,?,?,?)");
    event.text(1, impl_->meta.id);
    event.integer(2, impl_->seq);
    event.text(3, type);
    event.blob(4, encoded);
    event.done();

    if (begins) {
        const std::string title = title_from(payload);
        Statement update(impl_->db.get(),
                         "UPDATE sessions SET title=COALESCE(title,?),updated=?,open_turn=1 WHERE id=?");
        if (title.empty()) update.null(1); else update.text(1, title);
        update.integer(2, timestamp);
        update.text(3, impl_->meta.id);
        update.done();
    } else {
        Statement update(impl_->db.get(),
                         ends ? "UPDATE sessions SET updated=?,open_turn=0 WHERE id=?"
                              : "UPDATE sessions SET updated=? WHERE id=?");
        update.integer(1, timestamp);
        update.text(2, impl_->meta.id);
        update.done();
    }
    if (tx) tx->commit();
    ++impl_->seq;
}

void Writer::sync() {
    sqlite3_db_cacheflush(impl_->db.get());
}

const Meta& Writer::meta() const noexcept { return impl_->meta; }

std::vector<Summary> list(const Options& options, const fs::path& cwd, std::size_t limit) {
    Database db(options.database);
    Statement query(db.get(),
                    "SELECT id,title,model,created,updated FROM sessions WHERE cwd=? "
                    "AND (parent_id IS NULL OR parent_id='') ORDER BY updated DESC LIMIT ?");
    query.text(1, normalized(cwd).string());
    query.integer(2, limit == 0 ? -1 : static_cast<std::int64_t>(limit));
    std::vector<Summary> out;
    while (query.row()) {
        Summary summary;
        summary.meta.id = query.string(0);
        summary.title = query.is_null(1) ? std::string{} : query.string(1);
        summary.meta.cwd = normalized(cwd);
        summary.meta.model = query.string(2);
        summary.meta.created = iso_from_ms(query.integer(3));
        summary.updated = std::chrono::system_clock::time_point{std::chrono::milliseconds{query.integer(4)}};
        out.push_back(std::move(summary));
    }
    return out;
}

std::vector<Summary> list_children(const Options& options, std::string_view parent_id) {
    Database db(options.database);
    Statement query(db.get(),
                    "SELECT id,title,cwd,model,created,updated,agent_name FROM sessions "
                    "WHERE parent_id=? ORDER BY created ASC");
    query.text(1, parent_id);
    std::vector<Summary> out;
    while (query.row()) {
        Summary summary;
        summary.meta.id = query.string(0);
        summary.title = query.is_null(1) ? std::string{} : query.string(1);
        summary.meta.parent_id = std::string(parent_id);
        summary.meta.cwd = query.string(2);
        summary.meta.model = query.string(3);
        summary.meta.created = iso_from_ms(query.integer(4));
        summary.updated = std::chrono::system_clock::time_point{std::chrono::milliseconds{query.integer(5)}};
        if (!query.is_null(6)) summary.meta.agent_name = query.string(6);
        out.push_back(std::move(summary));
    }
    return out;
}

void replay(const Options& options, std::string_view id,
            const std::function<void(std::string_view, const json&)>& on_event) {
    Database db(options.database);
    (void)read_meta(db, id);
    Statement query(db.get(), "SELECT type,payload FROM events WHERE session_id=? ORDER BY seq");
    query.text(1, id);
    while (query.row()) {
        const std::string type = query.string(0);
        const std::string encoded = query.bytes(1);
        try {
            on_event(type, json::parse(encoded));
        } catch (const json::exception& error) {
            fail(SessionError::Kind::corrupt,
                 std::format("corrupt event payload in session {}: {}", id, error.what()));
        }
    }
}

} // namespace dagent::session
