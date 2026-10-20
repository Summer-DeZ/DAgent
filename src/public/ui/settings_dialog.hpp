#pragma once
#include "tui/runtime.hpp"
#include "ui/appearance.hpp"
#include <functional>

namespace dagent::ui {
class SettingsDialog final : public tui::EventHandler {
public:
    explicit SettingsDialog(tui::Runtime& rt) : rt_(rt) {}
    ~SettingsDialog() override;
    void open(Appearance, std::function<bool(const Appearance&)>,
              std::function<void(const Appearance&)>, int row = 0, std::function<void()> revert = {});
    void close();
    void saved();
    void failed();
    bool active() const { return overlay_ != 0; }
    void set_theme(const tui::ThemeTokens&);
    bool on_event(const tui::Event&) override;
private:
    class View;
    void cycle(int);
    void refresh();
    std::string value(int) const;
    tui::Runtime& rt_;
    View* view_ = nullptr;
    uint32_t overlay_ = 0;
    tui::ThemeTokens theme_ = tui::dark_theme();
    Appearance original_, current_;
    std::function<bool(const Appearance&)> preview_;
    std::function<void(const Appearance&)> save_;
    std::function<void()> revert_;
    int row_ = 0;
    bool editing_ = false, replace_hex_ = false, invalid_ = false, saving_ = false;
    bool background_custom_ = false, accent_custom_ = false, color_accepted_ = false;
    std::string hex_;
};
} // namespace dagent::ui
