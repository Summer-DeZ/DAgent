#pragma once

#include <functional>
#include <string>
#include <vector>

#include "tui/runtime.hpp"

namespace dagent::ui {

class Panel final : public tui::EventHandler {
public:
    struct Row {
        std::string label, middle, right;
        bool enabled = true;
        std::function<void()> activate;
    };

    explicit Panel(tui::Runtime&);
    ~Panel() override;
    void open(std::string title, std::vector<Row>, std::string hint,
              bool filter = true, std::function<void(int)> on_highlight = {},
              std::function<void(bool)> on_close = {}, int initial_row = 0);
    void close(bool committed = false);
    bool visible() const noexcept { return overlay_ != 0; }
    void set_theme(const tui::ThemeTokens&);
    bool on_event(const tui::Event&) override;

private:
    class View;
    void refilter();
    void move(int delta);

    tui::Runtime& rt_;
    View* view_ = nullptr;
    uint32_t overlay_ = 0;
    std::vector<Row> rows_;
    std::vector<int> shown_;
    std::string query_;
    bool filter_ = true;
    int selected_ = 0;
    std::function<void(int)> on_highlight_;
    std::function<void(bool)> on_close_;
    tui::ThemeTokens theme_ = tui::dark_theme();
};

} // namespace dagent::ui
