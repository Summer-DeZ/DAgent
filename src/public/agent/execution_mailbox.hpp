/// @file execution_mailbox.hpp
/// @brief Worker events consumed by the owning Dispatcher; handlers run without the queue lock.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace dagent::agent {

class ExecutionMailbox {
public:
    void start(std::size_t workers) {
        const std::lock_guard lock(mutex_);
        workers_ += workers;
    }
    void finished() {
        {
            const std::lock_guard lock(mutex_);
            --workers_;
        }
        changed_.notify_one();
    }
    void post(std::function<void()> event) {
        {
            const std::lock_guard lock(mutex_);
            events_.push_back(std::move(event));
        }
        changed_.notify_one();
    }
    void drain() {
        for (;;) {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return workers_ == 0 || !events_.empty(); });
            if (events_.empty()) return;
            auto event = std::move(events_.front());
            events_.pop_front();
            lock.unlock();
            event();
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> events_;
    std::size_t workers_ = 0;
};

} // namespace dagent::agent
