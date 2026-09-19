#pragma once
#include <functional>
#include "tui/input.hpp"

namespace dagent::ui {
class PromptInput final : public tui::EventHandler {
public:
    PromptInput(tui::InputBox& box, std::function<void(std::string)> submit,
                std::function<void()> recall);
    bool on_event(const tui::Event&) override;
private:
    tui::InputBox& box_;
    tui::InputBoxHandler edit_;
    std::function<void(std::string)> submit_;
    std::function<void()> recall_;
};
} // namespace dagent::ui
