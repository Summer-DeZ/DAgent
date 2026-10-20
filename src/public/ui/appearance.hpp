#pragma once

#include <array>
#include <filesystem>
#include <string>
#include "lib/nlohmann/json.hpp"
#include "tui/widget.hpp"

namespace dagent::ui {
struct Appearance {
    std::string theme = "dagent", mode = "auto", background = "solid", accent = "orange";
    std::string panels = "filled", density = "comfortable", icons = "unicode";
    std::string diff = "auto", links = "on";
    static Appearance from_json(const nlohmann::json&);
    nlohmann::json json() const;
};

struct Layout {
    nlohmann::json width = "auto";
    std::string status_panel = "right", navigator = "auto";
    std::vector<std::string> navigator_sections{"sessions", "git", "skills", "agents"};
    std::vector<std::string> status_sections{"session", "context", "plan", "changed", "running_agents", "mcp"};
    static Layout from_json(const nlohmann::json&);
    nlohmann::json json() const;
};
std::string_view section_name(std::string_view id);
const std::vector<std::string>& section_ids();

bool hex_color(std::string_view);
tui::ThemeTokens appearance_theme(const Appearance&, const std::filesystem::path& themes,
                                  const std::optional<tui::Color>& terminal);

// 应用层图标统一入口；改变集合后通过主题 epoch 使已物化块重新绘制。
struct Icons {
    std::string_view brand, branch, check, cross, current, dot, circle, folder, message, bar, meter, vertical, elbow;
    std::string_view horizontal, top_left, top_right, bottom_left, bottom_right, up, down, warning, minus, separator, enter, newline;
    std::array<std::string_view, 4> spinner;
    std::string_view toast_info, toast_warning;
    std::string_view right, chevron;
    std::string_view item, result, thought, user, todo, done;
};
const Icons& icons();
void set_icons(std::string_view);
std::string compact_tokens(std::size_t);
tui::Color diff_background(const tui::ThemeTokens&, bool added);
void dim_surface(tui::Surface&, const tui::ThemeTokens&, std::optional<tui::Color> terminal);
void draw_frame(tui::Surface&, tui::Rect, tui::Style);
void draw_hint(tui::Surface&, int x, int y, std::string_view, tui::Style, tui::Style separator);
int chip_width(std::string_view, bool outline);
int draw_chip(tui::Surface&, int x, int y, std::string_view, tui::Style,
              const tui::ThemeTokens&, bool outline, tui::Color background);

// 保留框架输入编辑/滚动，只在应用层重画可配置的框线。
class DialogInput : public tui::InputBox {
public:
    void set_theme(const tui::ThemeTokens& t) { theme_ = &t; tui::InputBox::set_theme(t); }
    void render(tui::Surface& s) override {
        tui::InputBox::render(s);
        if (s.cols() >= 3 && s.rows() >= 3) draw_frame(s, {0, 0, s.cols(), s.rows()}, theme_->border);
    }
private:
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
};

class Activity final : public tui::Widget {
public:
    void set_action(std::string value, std::string tokens = {}, std::string rate = {}) {
        action_ = std::move(value); tokens_ = std::move(tokens); rate_ = std::move(rate);
        invalidate_layout();
    }
    void tick() { frame_ = (frame_ + 1) % icons().spinner.size(); invalidate(); }
    void set_theme(const tui::ThemeTokens& theme) { theme_ = &theme; invalidate(); }
    tui::Size measure(tui::Size available) const override { return {available.cols, action_.empty() ? 0 : 1}; }
    void render(tui::Surface&) override;
private:
    std::string action_, tokens_, rate_;
    int frame_ = 0;
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
};
} // namespace dagent::ui
