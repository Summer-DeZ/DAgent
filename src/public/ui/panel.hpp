#pragma once

#include <functional>
#include <string>
#include <vector>

#include "tui/runtime.hpp"

namespace dagent::ui {

class Panel final : public tui::EventHandler {
public:
    enum class Tone { normal, muted, warning, error };
    struct Row {
        std::string label, middle, right;
        bool enabled = true;
        std::function<void()> activate;
        std::string group = {}, alias = {};
        Tone tone = Tone::normal, right_tone = Tone::muted;
        std::string search_text = {};
    };
    struct Presentation {
        int width = 76, height = 0;
        std::string status, footer;
        std::function<void(tui::Surface&, int)> details;
        std::function<void(int)> open_right;
        std::function<void()> refresh;
        bool close_on_activate = true;
        bool align_columns = false;
    };

    explicit Panel(tui::Runtime&);
    ~Panel() override;
    void open(std::string title, std::vector<Row>, std::string hint,
              bool filter = true, std::function<void(int)> on_highlight = {},
              std::function<void(bool)> on_close = {}, int initial_row = 0,
              std::function<void()> on_add = {}, std::string noun = {});
    void close(bool committed = false);
    bool visible() const noexcept { return overlay_ != 0; }
    void set_theme(const tui::ThemeTokens&);
    void present(Presentation);
    void update_rows(std::vector<Row>, bool reset_filter = false);
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
    std::string query_, noun_;
    bool filter_ = true;
    int selected_ = 0;
    std::function<void(int)> on_highlight_;
    std::function<void(bool)> on_close_;
    std::function<void()> on_add_;
    tui::ThemeTokens theme_ = tui::dark_theme();
    Presentation presentation_;
};

} // namespace dagent::ui
