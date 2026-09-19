#include "ui/status_line.hpp"
#include <format>

namespace dagent::ui {
void StatusLine::session(std::string model, std::string id) {
    model_ = std::move(model); id_ = std::move(id); used_ = limit_ = 0;
    mcp_text_.clear(); mcp_warning_ = false; invalidate();
}
void StatusLine::mcp(const std::vector<agent::ServerState>& states) {
    using Status = agent::ServerState::Status;
    std::size_t ready = 0;
    bool warning = false;
    std::string detail;
    for (const auto& state : states) {
        if (state.status == Status::ready) ++ready;
        else if (state.status == Status::failed || state.status == Status::disconnected) {
            warning = true;
            detail += std::format(" · {} {}", state.name,
                                   state.status == Status::failed ? "failed" : "disconnected");
        } else if (state.status == Status::connecting) detail += " · " + state.name + " 连接中";
        else if (state.status == Status::reconnecting) detail += " · " + state.name + " 重连中";
    }
    std::string text = states.empty() ? "" : std::format(" · MCP {}/{}{}", ready, states.size(), detail);
    if (text == mcp_text_ && warning == mcp_warning_) return;
    mcp_text_ = std::move(text); mcp_warning_ = warning; invalidate();
}
void StatusLine::context(const agent::ContextUpdate& e) { used_ = e.used; limit_ = e.limit; invalidate(); }
void StatusLine::permission(agent::PermissionMode mode) { mode_ = mode; invalidate(); }
void StatusLine::render(tui::Surface& surface) {
    surface.fill({0, 0, surface.cols(), surface.rows()}, U' ', theme_->background_panel);
    int col = surface.text(0, 0, model_ + " · ", theme_->text_muted);
    const double percent = limit_ ? 100.0 * used_ / limit_ : 0;
    col = surface.text(col, 0, limit_ ? std::format("上下文 {:.0f}%（{}/{}） · ", percent, used_, limit_)
                                    : "上下文 — · ",
                       percent >= trigger_ ? theme_->warning : theme_->text_muted);
    const bool edits = mode_ == agent::PermissionMode::accept_edits;
    col = surface.text(col, 0, edits ? "自动编辑" : "ask", edits ? theme_->accent : theme_->text_muted);
    col = surface.text(col, 0, mcp_text_, mcp_warning_ ? theme_->warning : theme_->text_muted);
    surface.text(col, 0, " · " + id_.substr(0, 8), theme_->text_muted);
}
} // namespace dagent::ui
