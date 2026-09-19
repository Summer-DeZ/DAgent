#pragma once
#include "agent/events.hpp"
#include "agent/mcp_hub.hpp"
#include "agent/permission.hpp"
#include "tui/widget.hpp"

namespace dagent::ui {
class StatusLine final : public tui::Widget {
public:
    void session(std::string model, std::string id);
    void context(const agent::ContextUpdate& update);
    void permission(agent::PermissionMode mode);
    void mcp(const std::vector<agent::ServerState>&);
    void set_theme(const tui::ThemeTokens& theme) { theme_ = &theme; invalidate(); }
    void set_trigger(int percent) { trigger_ = percent; }
    tui::Size measure(tui::Size available) const override { return {available.cols, 1}; }
    void render(tui::Surface&) override;
private:
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
    std::string model_, id_;
    std::string mcp_text_;
    bool mcp_warning_ = false;
    std::size_t used_ = 0, limit_ = 0;
    int trigger_ = 80;
    agent::PermissionMode mode_ = agent::PermissionMode::ask;
};
} // namespace dagent::ui
