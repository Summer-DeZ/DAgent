#include "ipc/channel.hpp"
#include "base/platform.hpp"

namespace dagent::ipc {

bool Channel::send_line(std::string_view line) {
    if (!valid()) {
        error_ = "channel is closed";
        return false;
    }
    std::string data(line);
    data += '\n';
    std::size_t offset = 0;
    while (offset < data.size()) {
        Transfer written = send_bytes(std::string_view(data).substr(offset));
        if (written.kind == Transfer::Kind::bytes) {
            offset += written.count;
            continue;
        }
        if (written.kind == Transfer::Kind::interrupted) continue;
        error_ = written.kind == Transfer::Kind::closed ? "connection closed" : std::move(written.error);
        return false;
    }
    return true;
}

std::optional<std::string> Channel::receive_line(std::size_t max_bytes) {
    for (;;) {
        if (const std::size_t newline = buffer_.find('\n'); newline != std::string::npos) {
            std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            return line;
        }
        if (buffer_.size() > max_bytes) {
            error_ = "line exceeds the maximum size";
            return std::nullopt;
        }
        if (!valid()) {
            error_ = "channel is closed";
            return std::nullopt;
        }
        char chunk[8192];
        Transfer read = receive_bytes(chunk);
        if (read.kind == Transfer::Kind::bytes) {
            buffer_.append(chunk, read.count);
            continue;
        }
        if (read.kind == Transfer::Kind::closed) {
            eof_ = true;
            return std::nullopt;
        }
        if (read.kind == Transfer::Kind::interrupted) continue;
        error_ = std::move(read.error);
        return std::nullopt;
    }
}

std::filesystem::path backend_binary_path() {
    std::error_code error;
    const std::filesystem::path self = base::executable_path(error);
    if (error) return {};
    const std::filesystem::path backend = self.parent_path() / "dagent-backend";
    if (!std::filesystem::exists(backend, error)) return {};
    return backend;
}

} // namespace dagent::ipc
