#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "tui/runtime.hpp"

namespace dagent::ui {

class ToastStack {
public:
    explicit ToastStack(tui::Runtime&);
    ~ToastStack();
    void show(tui::Notice::Severity, std::string text);
    void set_theme(const tui::ThemeTokens&);

private:
    class View;
    void expire(std::uint64_t id);

    tui::Runtime& rt_;
    View* view_ = nullptr;
    uint32_t overlay_ = 0;
    std::uint64_t next_id_ = 1;
    tui::ThemeTokens theme_ = tui::dark_theme();
};

} // namespace dagent::ui
