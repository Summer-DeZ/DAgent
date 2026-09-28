/// @brief Runtime 的公开状态、事件与错误值；不暴露调度实现。
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "agent/events.hpp"
#include "agent/mcp_state.hpp"
#include "agent/permission.hpp"
#include "agent/public_model.hpp"

namespace dagent::runtime {
/// @brief 一条仍在后端内存队列里的普通输入。
struct QueuedInput {
    std::string id;
    std::string text;
};

/// @brief 面向前端的只读状态；取值与事件投影在同一短锁边界发布。
struct RuntimeSnapshot {
    std::string session_id;
    std::uint64_t generation = 0;
    agent::PublicModel model;
    agent::PermissionMode permission_mode = agent::PermissionMode::workspace;
    bool planning = false;
    bool read_only = false;
    bool busy = false;
    std::string operation; ///< "turn" / "compact" / "replacing"；空表示空闲
    std::string run_id;
    std::vector<QueuedInput> queue;
    std::size_t used_tokens = 0;
    std::size_t token_limit = 0;
    std::size_t window_tokens = 0;
    int trigger_percent = 80;
    std::filesystem::path cwd, project_root;
    agent::TodoView work_plan;
    std::vector<agent::McpServerState> mcp;
    bool recording_broken = false;
    std::string recording_error;
    std::vector<agent::Policy::SessionGrant> grants;
};

/// @brief runtime 层的控制事件：不属于核心执行事实的状态变化与操作结果。
struct ControlEvent {
    enum class Kind {
        session_replaced,   ///< new/resume/切模型安装完成（前端重置或刷新历史）
        operation_finished, ///< 手动压缩结束（清 busy 并 drain）
        failed,             ///< 控制操作失败；operation 指出动作
    };
    Kind kind = Kind::session_replaced;
    std::string operation;
    std::string error;
    agent::TurnStatus status = agent::TurnStatus::done;
    bool replace_transcript = true; ///< session_replaced 时前端是否重置 Transcript（切模型为 false）
    bool resumed = false;           ///< session_replaced 来自显式恢复（前端显示恢复头并读取历史）
};

using EventPayload = std::variant<agent::Event, ControlEvent>;

struct Event {
    std::string session_id;
    std::uint64_t generation = 0;
    EventPayload payload;
};

struct RuntimeError {
    enum class Kind {
        busy,
        stale_session,
        not_found,
        session_in_use,
        invalid_state,
        config_error,
        query_failed,
        startup_failed,
        closing,
        interaction_closed,
    };
    Kind kind = Kind::invalid_state;
    std::string message;
};

struct StartOptions {
    std::optional<std::string> resume_id;
    bool continue_last = false;
};

} // namespace dagent::runtime
