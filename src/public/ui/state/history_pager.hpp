#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "protocol/dto.hpp"
#include "protocol/rpc.hpp"
#include "ui/state/callback_gate.hpp"

namespace dagent::client { class Client; }
namespace dagent::tui { class Runtime; }

namespace dagent::ui {

/// @brief 只读历史分页：每页结果经 rt.post 回到渲染线程；关闭/离开时释放未读完游标。
class HistoryPager : public std::enable_shared_from_this<HistoryPager> {
public:
    HistoryPager(client::Client& client, tui::Runtime& rt, std::shared_ptr<CallbackGate> gate,
                 std::string session_id);

    std::function<void(const protocol::HistoryItem&)> on_item;
    std::function<void(std::string)> on_error;
    std::function<void()> on_done;

    void start();
    void cancel();

private:
    void close_cursor(const std::string& cursor);
    void request();
    void handle(std::expected<nlohmann::json, protocol::RpcError> response);
    bool cancelled();

    client::Client& client_;
    tui::Runtime& rt_;
    std::shared_ptr<CallbackGate> gate_;
    std::string session_id_;
    std::mutex mutex_;
    std::string cursor_;
    bool cancelled_ = false;
};

std::vector<protocol::HistoryItem> load_history(client::Client& client, const std::string& session_id);

} // namespace dagent::ui
