/// @file memory_browser.hpp
/// @brief 记忆列表、查看与删除流程，使用宿主提供的共享 Panel。
#pragma once

#include <functional>
#include <string>

#include "protocol/rpc.hpp"
#include "ui/overlay/panel.hpp"

namespace dagent::ui {

/// 作为 Shell 成员使用；RPC 经 Shell 的 CallbackGate 在 UI 线程交付。
class MemoryBrowser {
public:
    using Done = std::function<void(const nlohmann::json&)>;
    using Failed = std::function<void(const protocol::RpcError&)>;
    using Request = std::function<void(std::string, nlohmann::json, Done, Failed)>;
    struct Actions {
        Request request;
        std::function<void(std::string, tui::Notice::Severity)> toast;
        std::function<int()> columns;
    };

    MemoryBrowser(Panel&, Actions);
    MemoryBrowser(const MemoryBrowser&) = delete;
    MemoryBrowser& operator=(const MemoryBrowser&) = delete;

    void open();

private:
    void show_error(const protocol::RpcError&);
    void show_entry(const std::string& scope, const std::string& name);

    Panel& panel_;
    Actions actions_;
};

} // namespace dagent::ui
