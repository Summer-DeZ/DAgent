/// @file workspace.hpp
/// @brief 主页面的容器与小控件：排队标签、子 Agent 标签、调暗基础层、三栏工作区与滚动区框架。
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "protocol/dto.hpp"
#include "tui/document/scrollback.hpp"
#include "tui/runtime/input.hpp"
#include "tui/widget/layout.hpp"
#include "tui/widget/widget.hpp"
#include "ui/page/side_panel.hpp"
#include "ui/page/welcome.hpp"

namespace dagent::ui {

class QueueLabel final : public tui::Widget {
public:
    void set_queue(const std::vector<protocol::QueueItem>& queue) { queue_ = queue; invalidate_layout(); }
    void set_theme(const tui::ThemeTokens& t) { theme_ = &t; invalidate(); }
    bool outline = false;
    tui::Size measure(tui::Size available) const override { return {available.cols, queue_.empty() ? 0 : 1}; }
    void render(tui::Surface& s) override;
private:
    std::vector<protocol::QueueItem> queue_;
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
};


class AgentTabs final : public tui::Widget, public tui::EventHandler {
public:
    std::vector<AgentInfo> tabs;
    size_t active = 0;
    int frame = 0;
    std::function<void(size_t)> select;
    void set_theme(const tui::ThemeTokens& t) { theme_ = &t; invalidate(); }
    tui::Size measure(tui::Size a) const override { return {a.cols, tabs.size() > 1 ? 2 : 0}; }
    bool on_event(const tui::Event& e) override;
    void render(tui::Surface& s) override;
private:
    struct Hit { int begin, end; size_t index; };
    std::vector<Hit> hits_;
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
};


// 重画完整基础层再混色，避免增量帧反复把已经变暗的前景再次混色。
class Page final : public tui::Container {
public:
    std::function<bool()> modal;
    std::function<void(tui::Surface&)> dim;
    bool dirty_tree() const noexcept override;
    void render(tui::Surface& surface) override;
private:
    bool dimmed_ = false;
};


class Workspace final : public tui::Container {
public:
    Workspace() : tui::Container(Direction::horizontal) {}
    bool left = false;
    std::function<void(int)> resize;
    void layout(tui::Rect area) override;
};


class ScrollFrame final : public tui::Widget {
public:
    WelcomeView welcome;
    ScrollFrame() { adopt(*this, welcome); }
    ~ScrollFrame() override { disown(welcome); if (current_ != nullptr) disown(*current_); }

    /// 新建一个滚动区；第一个自动成为当前显示的那个。
    tui::Scrollback& create();
    /// 切换显示的滚动区；旧的 disown、新的 adopt。
    void show(tui::Scrollback& scroll);
    /// 删除一个滚动区；调用方必须先解绑它的鼠标处理器。
    void destroy(tui::Scrollback& scroll);
    tui::Scrollback* current() const { return current_; }

    tui::Size measure(tui::Size available) const override { return available; }
    void layout(tui::Rect area) override;
    void render(tui::Surface& surface) override;
    bool dirty_tree() const noexcept override;
    bool needs_layout() const noexcept override;
    void invalidate_tree() noexcept override;
    void invalidate_rect(tui::Rect rect) override;
    tui::Widget* hit_test(tui::Point point) const noexcept override;
private:
    std::vector<std::unique_ptr<tui::Scrollback>> scrolls_;
    tui::Scrollback* current_ = nullptr;
};

} // namespace dagent::ui
