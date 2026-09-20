#include "ui/prompt_input.hpp"
#include <utility>

namespace dagent::ui {
PromptInput::PromptInput(tui::InputBox& box, std::function<void(std::string)> submit,
                         std::function<void()> recall, std::function<void()> changed,
                         std::function<bool()> modal_visible)
    : box_(box), edit_(box), submit_(std::move(submit)), recall_(std::move(recall)),
      changed_(std::move(changed)), modal_visible_(std::move(modal_visible)) {}

bool PromptInput::on_event(const tui::Event& e) {
    if (e.kind == tui::Event::Kind::key && e.mods == tui::Mods::none && box_.text().empty() &&
        (e.key == tui::Key::home || e.key == tui::Key::end)) return false;
    if (modal_visible_ && modal_visible_() &&
        (e.key == tui::Key::enter || e.key == tui::Key::tab ||
         e.key == tui::Key::up || e.key == tui::Key::down)) return false;
    if (e.kind == tui::Event::Kind::key && e.key == tui::Key::enter) {
        if (tui::any(e.mods & (tui::Mods::shift | tui::Mods::alt))) box_.insert("\n");
        else {
            std::string value = box_.text();
            if (value.find_first_not_of(" \t\r\n") != std::string::npos) {
                box_.set_text({}); submit_(std::move(value));
            }
        }
        return true;
    }
    if (e.kind == tui::Event::Kind::key && e.key == tui::Key::up &&
        e.mods == tui::Mods::none && box_.text().empty()) {
        recall_(); return true;
    }
    const bool handled = edit_.on_event(e);
    if (handled && changed_) changed_();
    return handled;
}
} // namespace dagent::ui
