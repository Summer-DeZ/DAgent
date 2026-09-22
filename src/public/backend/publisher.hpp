/// @file publisher.hpp
/// @brief 单一有序发送队列：事件序号与入队原子化；快照遇到并发事件时重新取值。
///
/// 所有协议输出（事件、响应、通知）都经这里；锁内不写 socket、不读数据库、不等待发送容量。
/// 容量不足时条件变量释放锁等待；取值、序号与入队在同一短锁边界。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "ipc/channel.hpp"
#include "lib/nlohmann/json.hpp"
#include "protocol/dto.hpp"
#include "runtime/controller.hpp"

namespace dagent::backend {

class Publisher {
public:
    /// on_error 在发送失败时调用一次（由发送线程调用，实现只做标记/唤醒）。
    Publisher(ipc::Channel& channel, std::function<void()> on_error);
    ~Publisher();
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    void start();
    /// 停止接收并等待发送线程退出；幂等。
    void stop();

    // 发送入口：按实际编码大小等待容量并入队。
    void send_line(std::string line);
    void send_event(protocol::Event event);
    /// @brief 在容量等待后读取快照；若其间有事件入队则重读，避免旧状态携带新 state_seq。
    void send_snapshot(const std::string& id,
                       const std::function<runtime::RuntimeSnapshot()>& capture);
    /// @brief 等待队列清空（或发送失败）。
    void flush();

    bool failed() const;
    const std::string& error() const { return error_; }

private:
    struct Item {
        std::string line;
        std::size_t bytes = 0;
    };

    void sender(std::stop_token stop);
    void push_locked(Item item);
    bool has_capacity_locked(std::size_t bytes) const;
    void enqueue_line_locked(std::string line);

    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<Item> queue_;
    std::size_t queued_bytes_ = 0;
    std::uint64_t seq_ = 0;
    bool stopping_ = false;
    bool failed_ = false;
    std::string error_;
    ipc::Channel& channel_;
    std::function<void()> on_error_;
    std::jthread thread_;
};

} // namespace dagent::backend
