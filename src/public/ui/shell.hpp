#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>

#include "client/client.hpp"
#include "protocol/dto.hpp"

namespace dagent::ui {

struct InteractiveOptions {
    std::string initial_prompt;
    std::filesystem::path theme_file;
    bool resumed = false;
    protocol::SessionSnapshot initial; ///< initialize 返回的权威快照
};

/// @brief 后端线程 → 渲染线程的事件桥。Client 在 initialize 前就要回调，
/// Shell 创建后 attach，退出前 detach；未 attach 时丢弃（此时还没有业务执行）。
class FrontendBridge {
public:
    FrontendBridge();
    ~FrontendBridge();
    FrontendBridge(const FrontendBridge&) = delete;
    FrontendBridge& operator=(const FrontendBridge&) = delete;

    void event(const protocol::Event&);
    void interaction_requested(const protocol::InteractionRequest&);
    void interaction_closed(const std::string& interaction_id);
    void disconnected(const std::string& message);

    // 只由 shell.cpp 的 run_interactive/Shell 调用。
    void attach(class Shell& shell);
    void detach();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 前端只持有 Client 与只读 DTO；业务命令发请求，页面状态留在本地。
int run_interactive(client::Client&, FrontendBridge&, const InteractiveOptions&, std::stop_token stop);

} // namespace dagent::ui
