#pragma once
#include <functional>
#include <string>
#include <vector>
#include "tui/widget.hpp"

namespace dagent::ui {
struct KeyHint { std::string key, description; bool operator==(const KeyHint&) const = default; };
class HintBar final : public tui::Widget {
public:
    std::string path;
    bool outline = false;
    std::function<std::vector<KeyHint>()> hints;
    void set_theme(const tui::ThemeTokens& t) { theme_ = &t; invalidate(); }
    tui::Size measure(tui::Size a) const override { return {a.cols, 1}; }
    bool dirty_tree() const noexcept override { return dirty_ || (hints && hints() != painted_); }
    void render(tui::Surface&) override;
private:
    std::vector<KeyHint> painted_;
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
};
} // namespace dagent::ui
