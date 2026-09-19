#include "session/session.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/json.hpp"
#include "base/log.hpp"

namespace dagent::session {
namespace fs = std::filesystem;
namespace {

constexpr std::string_view kEventsFile = "events.jsonl";
constexpr std::string_view kBlobDir = "blobs";
constexpr std::size_t kTitleReadBytes = 8 << 10;
constexpr std::size_t kTitleEvents = 20;

[[noreturn]] void fail(SessionError::Kind kind, const std::string& what) {
    throw SessionError(kind, what);
}

std::string errno_text(int err) { return std::strerror(err); }

std::string string_field(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::string now_iso8601() {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return std::format("{:%FT%T}Z", now);
}

fs::path default_directory() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        return fs::path(xdg) / "dagent" / "sessions";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return fs::path(home) / ".local" / "share" / "dagent" / "sessions";
    }
    fail(SessionError::Kind::io, "session directory is not configured and HOME/XDG_DATA_HOME are unset");
}

fs::path sessions_root(const Options& options) {
    return options.directory.empty() ? default_directory() : options.directory;
}

fs::path date_directory() {
    const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    return fs::path(std::format("{:%Y/%m/%d}", today));
}

void write_all(int fd, std::string_view data, const fs::path& path) {
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            fail(SessionError::Kind::io, std::format("write {}: {}", path.string(), errno_text(errno)));
        }
        written += static_cast<std::size_t>(n);
    }
}

void pread_all(int fd, char* data, std::size_t size, std::uint64_t offset, const fs::path& path) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::pread(fd, data + done, size - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            fail(SessionError::Kind::io, std::format("read {}: {}", path.string(), errno_text(errno)));
        }
        if (n == 0) fail(SessionError::Kind::corrupt, std::format("unexpected EOF: {}", path.string()));
        done += static_cast<std::size_t>(n);
    }
}

// 从文件末尾往前找最后一条完整合法的 JSON 行；返回应保留的字节数和该行的 seq。
// 被杀进程写了一半的行、以及没有换行结尾的最后一行，都在这里处理。
struct Recovery {
    std::uint64_t valid_bytes = 0;
    long long last_seq = -1;
    bool needs_newline = false;
};

Recovery recover_tail(int fd, std::uint64_t size, const fs::path& path) {
    if (size == 0) return {};
    std::uint64_t window_start = size;
    std::string buffer; // 覆盖 [window_start, size)
    while (true) {
        const std::uint64_t chunk = std::min<std::uint64_t>(64 << 10, window_start);
        window_start -= chunk;
        std::string part(chunk, '\0');
        pread_all(fd, part.data(), chunk, window_start, path);
        buffer.insert(0, part);

        bool need_more = false;
        std::size_t cursor = buffer.size();
        while (cursor > 0) {
            const bool has_newline = buffer[cursor - 1] == '\n';
            const std::size_t line_end = has_newline ? cursor - 1 : cursor;
            const std::size_t nl =
                line_end == 0 ? std::string::npos : buffer.rfind('\n', line_end - 1);
            const std::size_t line_start = nl == std::string::npos ? 0 : nl + 1;
            if (nl == std::string::npos && window_start != 0) {
                need_more = true; // 行比窗口还长，继续往前读
                break;
            }
            const std::string_view line(buffer.data() + line_start, line_end - line_start);
            const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) {
                Recovery out;
                out.valid_bytes = window_start + (has_newline ? line_end + 1 : line_end);
                out.last_seq = parsed.value("seq", -1LL);
                out.needs_newline = !has_newline;
                return out;
            }
            if (nl == std::string::npos) return {}; // 整个文件都没有合法行
            cursor = nl + 1;                        // 丢掉这一行（连同它的结尾换行），继续往前
        }
        if (!need_more || window_start == 0) return {};
    }
}

Meta parse_meta(const nlohmann::json& line, const fs::path& path) {
    if (string_field(line, "type") != "meta") {
        fail(SessionError::Kind::corrupt, std::format("missing meta line: {}", path.string()));
    }
    Meta meta;
    meta.id = string_field(line, "id");
    meta.cwd = string_field(line, "cwd");
    meta.git_root = string_field(line, "git_root");
    meta.model = string_field(line, "model");
    meta.created = string_field(line, "ts");
    if (meta.id.empty()) fail(SessionError::Kind::corrupt, std::format("meta without id: {}", path.string()));
    return meta;
}

Meta read_meta(const fs::path& dir) {
    const fs::path file = dir / kEventsFile;
    std::ifstream in(file, std::ios::binary);
    if (!in) fail(SessionError::Kind::io, std::format("cannot open {}", file.string()));
    std::string first;
    if (!std::getline(in, first) || first.empty()) {
        fail(SessionError::Kind::corrupt, std::format("empty session file: {}", file.string()));
    }
    const nlohmann::json line = nlohmann::json::parse(first, nullptr, false);
    if (line.is_discarded() || !line.is_object()) {
        fail(SessionError::Kind::corrupt, std::format("invalid meta line: {}", file.string()));
    }
    return parse_meta(line, file);
}

bool valid_id(std::string_view id) {
    return !id.empty() && id.find_first_of("/\\") == std::string_view::npos && id != "." && id != "..";
}

std::optional<fs::path> find_session_dir(const fs::path& root, std::string_view id) {
    if (!valid_id(id)) return std::nullopt;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return std::nullopt;
    for (const fs::directory_entry& year : fs::directory_iterator(root, ec)) {
        if (!year.is_directory()) continue;
        for (const fs::directory_entry& month : fs::directory_iterator(year.path(), ec)) {
            if (!month.is_directory()) continue;
            for (const fs::directory_entry& day : fs::directory_iterator(month.path(), ec)) {
                if (!day.is_directory()) continue;
                const fs::path candidate = day.path() / std::string(id);
                if (fs::is_directory(candidate, ec)) return candidate;
            }
        }
    }
    return std::nullopt;
}

std::vector<fs::path> all_session_dirs(const fs::path& root) {
    std::vector<fs::path> dirs;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return dirs;
    for (const fs::directory_entry& year : fs::directory_iterator(root, ec)) {
        if (!year.is_directory()) continue;
        for (const fs::directory_entry& month : fs::directory_iterator(year.path(), ec)) {
            if (!month.is_directory()) continue;
            for (const fs::directory_entry& day : fs::directory_iterator(month.path(), ec)) {
                if (!day.is_directory()) continue;
                for (const fs::directory_entry& entry : fs::directory_iterator(day.path(), ec)) {
                    if (entry.is_directory()) dirs.push_back(entry.path());
                }
            }
        }
    }
    return dirs;
}

// 大字符串转成 blob 引用：{"$blob":"blobs/000001.txt","bytes":N}
class BlobWriter {
public:
    BlobWriter(const Options& options, fs::path dir, std::uint64_t next_index)
        : options_(options), dir_(std::move(dir)), next_index_(next_index) {}

    nlohmann::json& split(nlohmann::json& value) {
        if (value.is_string()) {
            std::string& text = value.get_ref<std::string&>();
            if (text.size() > options_.max_inline_payload_bytes) value = store(text);
        } else if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) split(it.value());
        } else if (value.is_array()) {
            for (nlohmann::json& item : value) split(item);
        }
        return value;
    }

    std::uint64_t next_index() const { return next_index_; }

private:
    nlohmann::json store(std::string_view data) {
        const std::string name = std::format("{}/{:06}.txt", kBlobDir, next_index_++);
        const fs::path file = dir_ / name;
        const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) fail(SessionError::Kind::io, std::format("open {}: {}", file.string(), errno_text(errno)));
        try {
            write_all(fd, data, file);
            if (::fsync(fd) != 0) {
                fail(SessionError::Kind::io, std::format("fsync {}: {}", file.string(), errno_text(errno)));
            }
        } catch (...) {
            ::close(fd);
            throw;
        }
        if (::close(fd) != 0) {
            fail(SessionError::Kind::io, std::format("close {}: {}", file.string(), errno_text(errno)));
        }
        return nlohmann::json{{"$blob", name}, {"bytes", data.size()}};
    }

    const Options& options_;
    fs::path dir_;
    std::uint64_t next_index_;
};

void restore_blobs(nlohmann::json& value, const fs::path& session_dir,
                   std::optional<std::size_t> max_bytes = std::nullopt) {
    if (value.is_object()) {
        if (const auto it = value.find("$blob"); it != value.end() && it->is_string()) {
            const std::string rel = it->get<std::string>();
            if (rel.find("..") != std::string::npos || fs::path(rel).is_absolute()) {
                fail(SessionError::Kind::corrupt, std::format("unsafe blob reference: {}", rel));
            }
            const fs::path file = session_dir / rel;
            std::ifstream in(file, std::ios::binary);
            if (!in) fail(SessionError::Kind::io, std::format("cannot read blob {}", file.string()));
            if (max_bytes) {
                std::string prefix(*max_bytes, '\0');
                in.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
                prefix.resize(static_cast<std::size_t>(in.gcount()));
                value = std::move(prefix);
            } else {
                value = std::string((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
            }
            return;
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            restore_blobs(it.value(), session_dir, max_bytes);
        }
    } else if (value.is_array()) {
        for (nlohmann::json& item : value) restore_blobs(item, session_dir, max_bytes);
    }
}

std::uint64_t next_blob_index(const fs::path& session_dir) {
    std::uint64_t next = 1;
    std::error_code ec;
    const fs::path blobs = session_dir / kBlobDir;
    if (!fs::is_directory(blobs, ec)) return next;
    for (const fs::directory_entry& entry : fs::directory_iterator(blobs, ec)) {
        const std::string stem = entry.path().stem().string();
        if (stem.empty() || !std::all_of(stem.begin(), stem.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            continue;
        }
        next = std::max(next, static_cast<std::uint64_t>(std::stoull(stem)) + 1);
    }
    return next;
}

} // namespace

// ------------------------------------------------------------------ UUIDv7

std::string new_id() {
    static thread_local std::mt19937_64 rng([] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }());

    const auto milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const std::uint64_t rand_a = rng() & 0xFFF;
    const std::uint64_t rand_b = rng() & ((1ULL << 62) - 1);

    std::uint8_t bytes[16] = {};
    for (int i = 0; i < 6; ++i) bytes[i] = static_cast<std::uint8_t>(milliseconds >> (40 - 8 * i));
    bytes[6] = static_cast<std::uint8_t>(0x70 | ((rand_a >> 8) & 0x0F));
    bytes[7] = static_cast<std::uint8_t>(rand_a & 0xFF);
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

// ------------------------------------------------------------------- Writer

struct Writer::Impl {
    Options options;
    Meta meta;
    fs::path dir;
    int fd = -1;
    std::uint64_t seq = 1;
    std::uint64_t blob_index = 1;

    ~Impl() {
        if (fd >= 0) ::close(fd);
    }

    void write_line(const std::string& text) { write_all(fd, text, dir / kEventsFile); }
};

Writer::Writer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&&) noexcept = default;
Writer::~Writer() = default;

Writer Writer::create(const Options& options, Meta meta) {
    if (meta.id.empty()) meta.id = new_id();
    if (meta.created.empty()) meta.created = now_iso8601();

    const fs::path dir = sessions_root(options) / date_directory() / meta.id;
    std::error_code ec;
    fs::create_directories(dir / kBlobDir, ec);
    if (ec) fail(SessionError::Kind::io, std::format("create {}: {}", dir.string(), ec.message()));
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec); // 会话可能含敏感内容

    const fs::path file = dir / kEventsFile;
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) fail(SessionError::Kind::io, std::format("open {}: {}", file.string(), errno_text(errno)));

    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->meta = std::move(meta);
    impl->dir = dir;
    impl->fd = fd;
    impl->seq = 1;

    const nlohmann::json line = {{"type", "meta"},   {"seq", 0},
                                 {"ts", impl->meta.created}, {"v", 1},
                                 {"id", impl->meta.id}, {"cwd", impl->meta.cwd.string()},
                                 {"git_root", impl->meta.git_root.string()},
                                 {"model", impl->meta.model}, {"dagent_version", DAGENT_VERSION}};
    impl->write_line(line.dump() + '\n');
    return Writer(std::move(impl));
}

Writer Writer::resume(const Options& options, std::string_view id) {
    const fs::path root = sessions_root(options);
    const std::optional<fs::path> found = find_session_dir(root, id);
    if (!found) fail(SessionError::Kind::not_found, std::format("session not found: {}", id));

    const fs::path dir = *found;
    const fs::path file = dir / kEventsFile;
    const int fd = ::open(file.c_str(), O_RDWR | O_APPEND | O_CLOEXEC);
    if (fd < 0) fail(SessionError::Kind::io, std::format("open {}: {}", file.string(), errno_text(errno)));

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        fail(SessionError::Kind::io, std::format("stat {}: {}", file.string(), errno_text(errno)));
    }

    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->dir = dir;
    impl->fd = fd;

    const Recovery recovery = recover_tail(fd, static_cast<std::uint64_t>(st.st_size), file);
    if (recovery.valid_bytes < static_cast<std::uint64_t>(st.st_size)) {
        base::logger("session")->warn("truncating incomplete last line in {}", file.string());
        if (::ftruncate(fd, static_cast<off_t>(recovery.valid_bytes)) != 0) {
            fail(SessionError::Kind::io, std::format("truncate {}: {}", file.string(), errno_text(errno)));
        }
    }
    if (recovery.needs_newline) {
        write_all(fd, "\n", file);
    }
    impl->seq = static_cast<std::uint64_t>(std::max<long long>(recovery.last_seq + 1, 1));

    std::error_code ec;
    fs::create_directories(dir / kBlobDir, ec);
    if (ec) fail(SessionError::Kind::io, std::format("create {}: {}", dir.string(), ec.message()));
    impl->blob_index = next_blob_index(dir);
    impl->meta = read_meta(dir);
    if (impl->meta.id != id) {
        base::logger("session")->warn("session id mismatch: dir={} meta={}", impl->meta.id, id);
    }
    return Writer(std::move(impl));
}

void Writer::append(std::string_view type, nlohmann::json payload) {
    nlohmann::json line = {{"type", type}, {"seq", impl_->seq++}, {"ts", now_iso8601()}};
    if (impl_->options.record_payloads) {
        base::redact(payload, impl_->options.redact_fields);
        BlobWriter blobs(impl_->options, impl_->dir, impl_->blob_index);
        line["payload"] = blobs.split(payload);
        impl_->blob_index = blobs.next_index();
    }
    impl_->write_line(line.dump() + '\n');
}

void Writer::sync() {
    if (::fsync(impl_->fd) != 0) {
        fail(SessionError::Kind::io, std::format("fsync {}: {}", impl_->dir.string(), errno_text(errno)));
    }
}

const Meta& Writer::meta() const noexcept { return impl_->meta; }

// ------------------------------------------------------------- list / replay

std::vector<Summary> list(const Options& options, const std::optional<fs::path>& project_root,
                          std::size_t limit,
                          const std::function<std::string(const nlohmann::json&)>& title_of) {
    std::optional<fs::path> wanted;
    if (project_root) {
        std::error_code ec;
        wanted = fs::weakly_canonical(*project_root, ec);
        if (ec) wanted = project_root->lexically_normal();
    }

    std::vector<Summary> summaries;
    for (const fs::path& dir : all_session_dirs(sessions_root(options))) {
        const fs::path file = dir / kEventsFile;
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) continue;
        Summary summary;
        try {
            summary.meta = read_meta(dir);
        } catch (const SessionError&) {
            base::logger("session")->warn("skip unreadable session {}", dir.string());
            continue;
        }
        if (wanted) {
            std::error_code canon;
            const fs::path git = fs::weakly_canonical(summary.meta.git_root, canon);
            const fs::path cwd = fs::weakly_canonical(summary.meta.cwd, canon);
            const bool same_project = (!summary.meta.git_root.empty() && git == *wanted) ||
                                      (!summary.meta.cwd.empty() && cwd == *wanted);
            if (!same_project) continue;
        }
        const auto mtime = fs::last_write_time(file, ec);
        summary.updated = ec ? std::chrono::system_clock::time_point{} : std::chrono::clock_cast<std::chrono::system_clock>(mtime);

        if (title_of) {
            std::ifstream in(file, std::ios::binary);
            std::string head(kTitleReadBytes, '\0');
            in.read(head.data(), static_cast<std::streamsize>(head.size()));
            head.resize(static_cast<std::size_t>(in.gcount()));
            if (const std::size_t last_newline = head.rfind('\n'); last_newline != std::string::npos) {
                head.resize(last_newline + 1);
            } else {
                head.clear();
            }
            nlohmann::json events = nlohmann::json::array();
            std::istringstream lines(head);
            for (std::string line; std::getline(lines, line);) {
                nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object()) continue;
                if (string_field(parsed, "type") == "meta") continue;
                if (auto payload = parsed.find("payload"); payload != parsed.end()) {
                    // 标题最多只需要一小段文本；有界恢复 blob，避免 list 因超长输入读取大文件。
                    restore_blobs(*payload, dir, kTitleReadBytes);
                }
                events.push_back(parsed);
                if (events.size() >= kTitleEvents) break;
            }
            summary.title = title_of(events);
        }
        summaries.push_back(std::move(summary));
    }

    std::stable_sort(summaries.begin(), summaries.end(),
                     [](const Summary& a, const Summary& b) { return a.updated > b.updated; });
    if (limit > 0 && summaries.size() > limit) summaries.resize(limit);
    return summaries;
}

void replay(const Options& options, std::string_view id,
            const std::function<void(std::string_view, const nlohmann::json&)>& on_event) {
    const std::optional<fs::path> found = find_session_dir(sessions_root(options), id);
    if (!found) fail(SessionError::Kind::not_found, std::format("session not found: {}", id));

    const fs::path file = *found / kEventsFile;
    std::ifstream in(file, std::ios::binary);
    if (!in) fail(SessionError::Kind::io, std::format("cannot open {}", file.string()));

    std::string line;
    std::uint64_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (line.empty()) continue;
        nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            // 只有写到一半的最后一行可以容忍（进程被杀）；其他位置属于损坏
            if (in.eof()) {
                base::logger("session")->warn("skip truncated last line of {}", file.string());
                break;
            }
            fail(SessionError::Kind::corrupt,
                 std::format("{}:{}: invalid JSON line", file.string(), line_number));
        }
        const std::string type = string_field(parsed, "type");
        if (type == "meta") continue;
        nlohmann::json payload =
            parsed.contains("payload") ? std::move(parsed["payload"]) : nlohmann::json(nullptr);
        restore_blobs(payload, *found);
        on_event(type, payload);
    }
}

} // namespace dagent::session
