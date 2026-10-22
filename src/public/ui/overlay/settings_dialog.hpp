#pragma once
#include "tui/runtime/runtime.hpp"
#include "ui/style/appearance.hpp"
#include <functional>

namespace dagent::ui {
class SettingsDialog final : public tui::EventHandler {
public:
    explicit SettingsDialog(tui::Runtime& rt) : rt_(rt) {}
    ~SettingsDialog() override;
    void open(Appearance, Layout, std::function<bool(const Appearance&, const Layout&)>,
              std::function<void(const Appearance&, const Layout&)>, int row = 0, std::function<void()> revert = {});
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
    std::function<bool(const Appearance&, const Layout&)> preview_;
    std::function<void(const Appearance&, const Layout&)> save_;
    std::function<void()> revert_;
    Layout original_layout_, layout_;
    std::vector<std::string> hidden_;
    std::vector<std::string>& column(int);
    void layout_event(const tui::Event&);
    bool layout_page_ = false, width_custom_ = false;
    int column_ = 0;
    int row_ = 0;
    bool editing_ = false, replace_hex_ = false, invalid_ = false, saving_ = false;
    bool background_custom_ = false, accent_custom_ = false, color_accepted_ = false;
    std::string hex_;
};
} // namespace dagent::ui
