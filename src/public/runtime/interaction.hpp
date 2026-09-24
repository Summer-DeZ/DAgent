/// @file interaction.hpp
/// @brief InteractionBroker：审批/问答等待的一次性终结与单模态排队。
///
/// 核心只请求交互；等待期间不持有队列锁、Policy 锁或控制器锁。回答/取消走即时路径，
/// 不经过被 run_turn 阻塞的业务队列（状态机 §6/§9）。前端出口由 UI/协议适配实现。
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>

#include "agent/events.hpp"

namespace dagent::runtime {

struct InteractionRequest {
    enum class Kind { approval, question };
    std::string id;
    Kind kind = Kind::approval;
    agent::Approval approval; ///< kind==approval
    agent::Question question; ///< kind==question
    std::string session_id;
    std::uint64_t generation = 0;
};

/// @brief 前端交互出口。方法从后端执行线程调用，实现负责转到自己的线程，不得阻塞。
class InteractionOutlet {
public:
    virtual ~InteractionOutlet() = default;

    /// @brief 一个交互被激活（单模态队列的队首），前端展示对话框。
    virtual void interaction_requested(const InteractionRequest&) = 0;
    /// @brief 已展示的交互被取消/结束时通知前端关闭对应对话框；重复关闭无副作用。
    virtual void interaction_closed(const std::string& interaction_id) = 0;
};

class InteractionBroker {
public:
    InteractionBroker() = default;
    ~InteractionBroker();
    InteractionBroker(const InteractionBroker&) = delete;
    InteractionBroker& operator=(const InteractionBroker&) = delete;

    void set_outlet(InteractionOutlet* outlet);

    /// @brief 执行线程：请求一次审批；取消时返回 deny（调用方按 stop 语义收尾）。
    agent::Decision request_approval(agent::Approval approval, std::string session_id,
                                     std::uint64_t generation, std::stop_token stop);
    /// @brief 执行线程：请求一次问答；取消时返回 cancelled。
    agent::Answer request_answer(agent::Question question, std::string session_id,
                                 std::uint64_t generation, std::stop_token stop);

    /// @brief 任意线程：回答一个仍 pending 的交互；已结束返回 false。
    bool answer_approval(const std::string& interaction_id, agent::Decision decision);
    bool answer_question(const std::string& interaction_id, agent::Answer answer);

    /// @brief 关闭流程：取消全部 Pending 与排队请求，唤醒所有等待线程。
    void cancel_all();

private:
    struct Pending;
    enum class Kind { approval, question };

    std::shared_ptr<Pending> request(Kind kind, agent::Approval approval, agent::Question question,
                                     std::string session_id, std::uint64_t generation,
                                     std::stop_token stop);
    /// @brief 在锁内结束一个 Pending 并返回下一个需要激活的请求；调用方在锁外通知出口。
    std::shared_ptr<Pending> complete_locked(const std::shared_ptr<Pending>& pending, bool activate_next);
    static InteractionRequest make_event(const std::shared_ptr<Pending>& pending);

    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<Pending>> waiting_;
    std::shared_ptr<Pending> active_;
    InteractionOutlet* outlet_ = nullptr;
    std::uint64_t next_interaction_ = 0;
    bool closed_ = false;
};

} // namespace dagent::runtime
