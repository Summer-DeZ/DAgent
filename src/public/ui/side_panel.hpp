#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "agent/tool_data.hpp"
#include "tui/widget.hpp"

namespace dagent::ui {

/// 右侧常驻信息栏：会话标题、上下文用量、当前计划、项目与版本。
/// 终端过窄或用户收起时 measure 返回宽 0，整行让给主区。
class SidePanel final : public tui::Widget {
public:
    void set_title(std::string);
    void set_context(std::size_t used, std::size_t limit);
    void set_items(std::vector<agent::TodoItem>);
    void set_mcp(std::string);
    void set_project(std::string path, std::string branch);
    void set_version(std::string);
    void set_collapsed(bool);

    [[nodiscard]] bool collapsed() const noexcept { return collapsed_; }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::pair<int, int> progress() const noexcept;
    void set_theme(const tui::ThemeTokens& theme) { theme_ = &theme; invalidate(); }

    tui::Size measure(tui::Size available) const override;
    void render(tui::Surface&) override;

private:
    std::vector<agent::TodoItem> items_;
    std::string title_, mcp_, project_, version_;
    std::size_t used_ = 0, limit_ = 0;
    bool collapsed_ = false;
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
};

} // namespace dagent::ui
