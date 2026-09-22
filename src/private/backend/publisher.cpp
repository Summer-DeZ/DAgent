#include "backend/publisher.hpp"

#include <limits>
#include <utility>

#include "backend/convert.hpp"

namespace dagent::backend {
namespace {

constexpr std::size_t kSoftCap = 8u << 20; ///< 8 MiB 软上限

} // namespace

Publisher::Publisher(ipc::Channel& channel, std::function<void()> on_error)
    : channel_(channel), on_error_(std::move(on_error)) {}

Publisher::~Publisher() { stop(); }

void Publisher::start() {
    thread_ = std::jthread([this](std::stop_token stop) { sender(stop); });
}

void Publisher::stop() {
    if (!thread_.joinable()) return;
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    thread_.request_stop();
    thread_.join();
    {
        const std::lock_guard lock(mutex_);
        if (failed_ || queue_.empty() || channel_.error().empty()) return;
        failed_ = true;
        error_ = channel_.error();
    }
}

bool Publisher::has_capacity_locked(std::size_t bytes) const {
    // 单条大消息允许在队列为空时单独发送。
    return queue_.empty() || (bytes <= kSoftCap && queued_bytes_ <= kSoftCap - bytes);
}

void Publisher::push_locked(Item item) {
    queued_bytes_ += item.bytes;
    queue_.push_back(std::move(item));
    cv_.notify_all();
}

void Publisher::enqueue_line_locked(std::string line) {
    Item item;
    item.line = std::move(line);
    item.bytes = item.line.size() + 1;
    push_locked(std::move(item));
}

void Publisher::send_line(std::string line) {
    const std::size_t bytes = line.size() + 1;
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return stopping_ || has_capacity_locked(bytes); });
    if (stopping_) return;
    enqueue_line_locked(std::move(line));
}

void Publisher::send_event(protocol::Event event) {
    event.seq = std::numeric_limits<std::uint64_t>::max();
    const std::size_t bytes = protocol::encode_notification(
                                  protocol::Notification{"event", nlohmann::json(event)}).size() + 1;
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return stopping_ || has_capacity_locked(bytes); });
    if (stopping_) return;
    event.seq = ++seq_;
    enqueue_line_locked(protocol::encode_notification(
        protocol::Notification{"event", nlohmann::json(event)}));
}

void Publisher::send_snapshot(const std::string& id,
                              const std::function<runtime::RuntimeSnapshot()>& capture) {
    for (;;) {
        std::uint64_t observed_seq;
        {
            const std::lock_guard lock(mutex_);
            if (stopping_) return;
            observed_seq = seq_;
        }
        protocol::SessionSnapshot dto = to_protocol(capture());
        dto.state_seq = std::numeric_limits<std::uint64_t>::max();
        const std::size_t bytes = protocol::encode_result(id, nlohmann::json(dto)).size() + 1;
        std::unique_lock lock(mutex_);
        if (stopping_) return;
        if (!has_capacity_locked(bytes)) {
            cv_.wait(lock, [&] { return stopping_ || has_capacity_locked(bytes); });
            if (stopping_) return;
            continue; // 等待期间状态可能改变，重新取值。
        }
        if (seq_ != observed_seq) continue;
        dto.state_seq = seq_;
        enqueue_line_locked(protocol::encode_result(id, nlohmann::json(std::move(dto))));
        return;
    }
}

void Publisher::flush() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return stopping_ || (queue_.empty() && !failed_); });
}

bool Publisher::failed() const {
    const std::lock_guard lock(mutex_);
    return failed_;
}

void Publisher::sender(std::stop_token stop) {
    for (;;) {
        std::string line;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, stop, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_) return;
                continue;
            }
            line = std::move(queue_.front().line);
            queued_bytes_ -= queue_.front().bytes;
            queue_.pop_front();
            cv_.notify_all();
        }
        if (!channel_.send_line(line)) {
            {
                const std::lock_guard lock(mutex_);
                failed_ = true;
                error_ = channel_.error();
                stopping_ = true;
            }
            cv_.notify_all();
            if (on_error_) on_error_();
            return;
        }
    }
}

} // namespace dagent::backend
