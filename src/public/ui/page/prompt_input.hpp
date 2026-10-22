#pragma once
#include <functional>
#include "tui/runtime/input.hpp"

namespace dagent::ui {
class PromptInput final : public tui::EventHandler {
public:
    PromptInput(tui::InputBox& box, std::function<void(std::string)> submit,
                std::function<void()> recall, std::function<void()> changed = {},
                std::function<bool()> modal_visible = {});
    std::function<bool(const tui::Event&)> before_edit;
    bool on_event(const tui::Event&) override;
private:
    tui::InputBox& box_;
    tui::InputBoxHandler edit_;
    std::function<void(std::string)> submit_;
    std::function<void()> recall_;
    std::function<void()> changed_;
    std::function<bool()> modal_visible_;
};
} // namespace dagent::ui
