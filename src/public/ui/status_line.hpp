#pragma once
#include <cstddef>
#include <string>

#include "tui/widget.hpp"

namespace dagent::ui {

/// 底部一行：左边项目路径，右边上下文用量与命令面板提示。
/// 模型、模式、MCP 与会话信息在右侧 SidePanel 与输入框尾行里显示。
class StatusLine final : public tui::Widget {
public:
    void project(std::string path);
    void context(std::size_t used, std::size_t limit);
    void todo(int done, int total, bool shown);
    void set_theme(const tui::ThemeTokens& theme) { theme_ = &theme; invalidate(); }
    void set_trigger(int percent) { if (trigger_ != percent) { trigger_ = percent; invalidate(); } }
    tui::Size measure(tui::Size available) const override { return {available.cols, 1}; }
    void render(tui::Surface&) override;

private:
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
    std::string path_;
    std::size_t used_ = 0, limit_ = 0;
    int trigger_ = 80;
    int todo_done_ = 0, todo_total_ = 0;
    bool todo_shown_ = false;
};

} // namespace dagent::ui
