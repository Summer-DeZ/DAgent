#include "storage/storage.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "agent/port_store.hpp"
#include "base/json.hpp"
#include "storage/history_read.hpp"
#include "base/log.hpp"
#include "base/text.hpp"
#include "lib/sqlite/sqlite3.h"

namespace dagent::storage {
namespace fs = std::filesystem;
namespace {

using nlohmann::json;

[[noreturn]] void fail(StorageError::Kind kind, const std::string& what) {
    throw StorageError(kind, what);
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
        if (rc != SQLITE_OK) fail(StorageError::Kind::io, sqlite3_errmsg(db_));
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
        if (row()) fail(StorageError::Kind::corrupt, "unexpected row from SQLite statement");
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
        if (rc != SQLITE_OK) fail(StorageError::Kind::io, sqlite3_errmsg(db_));
    }
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

enum class Access { read_write, read_only };

class Database {
public:
    explicit Database(const fs::path& file, Access access = Access::read_write) : file_(file) {
        if (file_.empty()) fail(StorageError::Kind::io, "session database path is empty");
        if (access == Access::read_only) {
            // 只读查询不初始化或修复数据库：不存在直接报告，损坏直接报错。
            std::error_code ec;
            if (!fs::exists(file_, ec)) fail(StorageError::Kind::not_found, "session database does not exist");
            open(true);
            return;
        }
        open(false);
        try {
            initialize();
        } catch (const StorageError& error) {
            sqlite3_close(db_);
            db_ = nullptr;
            const fs::path backup = file_.string() + std::format(".corrupt-{}", now_ms());
            std::error_code ec;
            fs::rename(file_, backup, ec);
            if (ec) throw;
            base::logger("session")->error("session database was unreadable; moved it to {} and created a new database: {}",
                                           backup.string(), error.what());
            open(false);
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
            fail(StorageError::Kind::io, text);
        }
    }

private:
    void open(bool read_only) {
        const int flags = (read_only ? SQLITE_OPEN_READONLY
                                     : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) |
                          SQLITE_OPEN_FULLMUTEX;
        const int rc = sqlite3_open_v2(file_.c_str(), &db_, flags, nullptr);
        if (rc != SQLITE_OK) {
            const std::string text = db_ == nullptr ? "cannot open SQLite database" : sqlite3_errmsg(db_);
            if (db_ != nullptr) sqlite3_close(db_);
            db_ = nullptr;
            fail(StorageError::Kind::io, text);
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
    if (!query.row()) fail(StorageError::Kind::not_found, "session not found: " + std::string(id));
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
    std::error_code ec;
    if (!fs::exists(options.database, ec)) return {};
    Database db(options.database, Access::read_only);
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
    std::error_code ec;
    if (!fs::exists(options.database, ec)) return {};
    Database db(options.database, Access::read_only);
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
    Database db(options.database, Access::read_only);
    (void)read_meta(db, id);
    Statement query(db.get(), "SELECT type,payload FROM events WHERE session_id=? ORDER BY seq");
    query.text(1, id);
    while (query.row()) {
        const std::string type = query.string(0);
        const std::string encoded = query.bytes(1);
        try {
            on_event(type, json::parse(encoded));
        } catch (const json::exception& error) {
            fail(StorageError::Kind::corrupt,
                 std::format("corrupt event payload in session {}: {}", id, error.what()));
        }
    }
}

// ---- 写租约 ----

namespace {

fs::path lock_path(const Options& options, std::string_view session_id) {
    return options.database.parent_path() / ".runtime" / "session-locks" /
           (std::string(session_id) + ".lock");
}

} // namespace

struct SessionWriteLease::State {
    std::string session_id;
    int fd = -1;

    ~State() {
        if (fd >= 0) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
        }
    }
};

namespace {

std::mutex& lease_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, std::weak_ptr<void>>& lease_registry() {
    static std::map<std::string, std::weak_ptr<void>> registry;
    return registry;
}

} // namespace

SessionWriteLease::SessionWriteLease(std::shared_ptr<State> state) : state_(std::move(state)) {}
SessionWriteLease::~SessionWriteLease() = default;

std::shared_ptr<SessionWriteLease> SessionWriteLease::acquire(const Options& options,
                                                              std::string_view session_id) {
    const fs::path path = lock_path(options, session_id);
    const std::string key = path.string();
    const std::lock_guard lock(lease_mutex());
    auto& registry = lease_registry();
    if (const auto it = registry.find(key); it != registry.end()) {
        if (auto held = it->second.lock()) {
            // 同进程（同会话切模型）复用同一个 flock，不再打开第二个文件描述符。
            return std::shared_ptr<SessionWriteLease>(
                new SessionWriteLease(std::static_pointer_cast<State>(held)));
        }
        registry.erase(it);
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) fail(StorageError::Kind::io, std::format("cannot create session lock directory: {}", ec.message()));
    auto state = std::make_shared<State>();
    state->session_id = std::string(session_id);
    state->fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (state->fd < 0) {
        fail(StorageError::Kind::io,
             std::format("cannot open session lock {}: {}", path.string(), std::strerror(errno)));
    }
    if (::flock(state->fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(state->fd);
        state->fd = -1;
        fail(StorageError::Kind::io,
             std::format("session {} is already in use by another process", session_id));
    }
    registry[key] = state;
    return std::shared_ptr<SessionWriteLease>(new SessionWriteLease(std::move(state)));
}

const std::string& SessionWriteLease::session_id() const noexcept { return state_->session_id; }

// ---- JournalWriter / SessionStore 实现 ----

namespace {

agent::RecordError to_record_error(const StorageError& error) {
    switch (error.kind()) {
    case StorageError::Kind::io:
    case StorageError::Kind::invalid_state: return agent::RecordError(agent::RecordError::Kind::io, error.what());
    case StorageError::Kind::not_found: return agent::RecordError(agent::RecordError::Kind::not_found, error.what());
    case StorageError::Kind::corrupt: return agent::RecordError(agent::RecordError::Kind::corrupt, error.what());
    }
    return agent::RecordError(agent::RecordError::Kind::io, error.what());
}

class SqliteJournal final : public agent::JournalWriter {
public:
    explicit SqliteJournal(Writer writer) : writer_(std::move(writer)) {}

    void append(const agent::Record& record) override {
        try {
            writer_.append(record.type, record.payload);
        } catch (const StorageError& error) {
            throw to_record_error(error);
        }
    }
    void sync() override {
        try {
            writer_.sync();
        } catch (const StorageError& error) {
            throw to_record_error(error);
        }
    }
    const Meta& meta() const override { return writer_.meta(); }

private:
    Writer writer_;
};

class SqliteSessionStore final : public agent::SessionStore {
public:
    explicit SqliteSessionStore(Options options) : options_(std::move(options)) {}

    std::vector<agent::StoredRecord> read_records(std::string_view session_id, std::int64_t from_seq,
                                                  std::int64_t to_seq) override {
        Database db(options_.database, Access::read_only);
        (void)read_meta(db, session_id);
        Statement query(db.get(),
                        "SELECT seq,type,payload FROM events WHERE session_id=? AND seq>=? AND seq<? "
                        "ORDER BY seq");
        query.text(1, session_id);
        query.integer(2, from_seq);
        query.integer(3, to_seq);
        std::vector<agent::StoredRecord> records;
        while (query.row()) {
            agent::StoredRecord record;
            record.seq = query.integer(0);
            record.type = query.string(1);
            const std::string encoded = query.bytes(2);
            try {
                record.payload = json::parse(encoded);
            } catch (const json::exception& error) {
                fail(StorageError::Kind::corrupt,
                     std::format("corrupt event payload in session {}: {}", session_id, error.what()));
            }
            records.push_back(std::move(record));
        }
        return records;
    }

    std::int64_t max_seq(std::string_view session_id) override {
        Database db(options_.database, Access::read_only);
        (void)read_meta(db, session_id);
        Statement query(db.get(), "SELECT COALESCE(MAX(seq),-1) FROM events WHERE session_id=?");
        query.text(1, session_id);
        return query.row() ? query.integer(0) : -1;
    }

    std::unique_ptr<agent::JournalWriter> open_writer_create(const agent::SessionMeta& meta) override {
        return std::make_unique<SqliteJournal>(Writer::create(options_, meta));
    }
    std::unique_ptr<agent::JournalWriter> open_writer_resume(std::string_view session_id) override {
        return std::make_unique<SqliteJournal>(Writer::resume(options_, session_id));
    }

private:
    Options options_;
};

} // namespace

std::unique_ptr<agent::SessionStore> open_store(const Options& options) {
    return std::make_unique<SqliteSessionStore>(options);
}

// ---- 只读历史分页 ----

namespace {
constexpr std::size_t kMaxScanPerPage = 100;
} // namespace

struct HistoryRead::Impl {
    Options options;
    Meta meta;
    std::int64_t upper_seq = -1;
    std::int64_t next_seq = 0;
    bool released = false;
    agent::HistoryCursor cursor;
    agent::HistoryProjector projector;
    std::unique_ptr<Database> db;

    void release() {
        released = true;
        db.reset();
    }
};

HistoryRead::HistoryRead(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
HistoryRead::~HistoryRead() = default;

std::unique_ptr<HistoryRead> HistoryRead::open(const Options& options, std::string_view session_id) {
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->db = std::make_unique<Database>(options.database, Access::read_only);
    impl->meta = read_meta(*impl->db, session_id);
    Statement query(impl->db->get(), "SELECT COALESCE(MAX(seq),-1) FROM events WHERE session_id=?");
    query.text(1, session_id);
    impl->upper_seq = query.row() ? query.integer(0) : -1;
    return std::unique_ptr<HistoryRead>(new HistoryRead(std::move(impl)));
}

HistoryRead::Page HistoryRead::read(const std::string& cursor, std::size_t limit) {
    Impl& impl = *impl_;
    if (impl.released) {
        fail(StorageError::Kind::invalid_state, "history query is closed");
    }
    if (!cursor.empty()) {
        try {
            const auto value = std::stoll(cursor);
            if (value != impl.next_seq) fail(StorageError::Kind::invalid_state, "unknown history cursor");
        } catch (const std::invalid_argument&) {
            fail(StorageError::Kind::invalid_state, "unknown history cursor");
        } catch (const std::out_of_range&) {
            fail(StorageError::Kind::invalid_state, "unknown history cursor");
        }
    }

    try {
        Page page;
        const std::size_t scan = std::min(limit == 0 ? kMaxScanPerPage : limit, kMaxScanPerPage);
        Statement query(impl.db->get(),
                        "SELECT seq,type,payload FROM events WHERE session_id=? AND seq>=? AND seq<? "
                        "ORDER BY seq LIMIT ?");
        query.text(1, impl.meta.id);
        query.integer(2, impl.next_seq);
        query.integer(3, impl.upper_seq + 1);
        query.integer(4, static_cast<std::int64_t>(scan));
        while (query.row()) {
            const std::int64_t seq = query.integer(0);
            const std::string type = query.string(1);
            const std::string encoded = query.bytes(2);
            json payload;
            try {
                payload = json::parse(encoded);
            } catch (const json::exception& error) {
                fail(StorageError::Kind::corrupt,
                     std::format("corrupt event payload in session {}: {}", impl.meta.id, error.what()));
            }
            agent::StoredRecord stored{seq, type, std::move(payload)};
            const auto decoded = agent::record_codec::decode(stored.type, stored.payload);
            impl.cursor.observe(decoded);
            for (agent::HistoryItem& item : impl.projector.project(stored, decoded)) {
                page.items.push_back(std::move(item));
            }
            impl.next_seq = seq + 1;
        }
        page.done = impl.next_seq > impl.upper_seq;
        if (page.done) {
            impl.release();
        } else {
            page.cursor = std::to_string(impl.next_seq);
        }
        return page;
    } catch (...) {
        impl.release(); // 查询失败时释放该查询资源
        throw;
    }
}

void HistoryRead::close() {
    if (impl_) impl_->release();
}

const Meta& HistoryRead::meta() const { return impl_->meta; }

std::int64_t HistoryRead::upper_seq() const { return impl_->upper_seq; }

} // namespace dagent::storage
