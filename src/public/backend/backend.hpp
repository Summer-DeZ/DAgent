/// @file backend.hpp
/// @brief dagent-backend 的 RPC 适配：方法分发、事件 DTO 转换与单一发送队列。
///
/// 读线程只做校验/即时操作（取消、回答、快照）；可耗时命令进命令线程；
/// 只读查询进查询线程。协议错误在这里映射，Runtime 业务错误不做协议包装。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "backend/publisher.hpp"
#include "ipc/channel.hpp"
#include "lib/nlohmann/json.hpp"
#include "protocol/rpc.hpp"
#include "runtime/factory.hpp"
#include "runtime/runtime.hpp"

namespace dagent::backend {

class Backend final : public runtime::Frontend {
public:
    /// @param assemble 由后端入口注入的 app 装配实现；backend 不依赖具体配置与适配对象。
    Backend(ipc::Channel channel, runtime::Assembler assemble);
    ~Backend() override;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    /// @brief 阻塞运行到 shutdown 或连接结束；返回进程退出码。
    int run();

    void event(const runtime::Event& event) override;
    void interaction_requested(const runtime::InteractionRequest& request) override;
    void interaction_closed(const std::string& interaction_id) override;

private:
    struct Job {
        std::string id;
        std::string method;
        nlohmann::json params;
    };

    void dispatch(const protocol::Request& request);
    void handle_command(Job job);
    void handle_query(Job job);

    void initialize(const std::string& id, const nlohmann::json& params);
    void new_session(const std::string& id, const nlohmann::json& params);
    void resume_session(const std::string& id, const nlohmann::json& params);
    void select_model(const std::string& id, const nlohmann::json& params);
    void add_model(const std::string& id, const nlohmann::json& params);
    void compact(const std::string& id, const nlohmann::json& params);
    void quit();

    void list_sessions(const std::string& id, const nlohmann::json& params);
    void list_children(const std::string& id, const nlohmann::json& params);
    void history(const std::string& id, const nlohmann::json& params);
    void history_close(const std::string& id, const nlohmann::json& params);
    void workspace_info(const std::string& id, const nlohmann::json& params);
    void workspace_complete(const std::string& id, const nlohmann::json& params);

    void handle_control(const runtime::ControlEvent& control, const runtime::Event& event);
    void handle_core(const runtime::Event& event);

    bool check_target(const std::string& id, const nlohmann::json& params);
    void respond(const std::string& id, nlohmann::json result);
    void respond_snapshot(const std::string& id);
    void fail(const std::string& id, const protocol::RpcError& error);
    void fail_target(const std::string& id, const runtime::RuntimeError& error);
    void push_command(Job job);
    void push_query(Job job);

    ipc::Channel channel_;
    runtime::Assembler assemble_;
    std::unique_ptr<Publisher> publisher_;
    std::unique_ptr<runtime::Runtime> runtime_;
    std::shared_ptr<runtime::QueryGateway> queries_;
    std::shared_ptr<runtime::ConfigurationGateway> configuration_;
    bool initialized_ = false;
    std::string mode_;
    std::string default_model_;
    int progress_interval_ms_ = 1000;

    std::atomic<bool> closing_{false};
    std::mutex command_mutex_;
    std::condition_variable_any command_cv_;
    std::deque<Job> command_jobs_;
    bool command_closed_ = false;
    std::jthread command_thread_;

    std::mutex query_mutex_;
    std::condition_variable_any query_cv_;
    std::deque<Job> query_jobs_;
    bool query_closed_ = false;
    std::jthread query_thread_;

    // 查询线程独占；每一页使用连接内唯一、单次消费的游标。
    std::map<std::string, std::unique_ptr<runtime::HistoryReader>> histories_;
    std::uint64_t history_seq_ = 0;
    std::mutex state_mutex_; ///< pending compact 身份
    std::string pending_compact_id_;
    std::uint64_t compact_seq_ = 0;

    std::mutex chunk_mutex_; ///< 每个 bash 调用的 UTF-8 边界缓冲
    std::map<std::string, std::string> pending_chunks_;
};

} // namespace dagent::backend
