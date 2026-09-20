#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "ui/prompt_box.hpp"
#include "tui/runtime.hpp"

namespace dagent::ui {

class Completion final : public tui::EventHandler {
public:
    struct Item {
        std::string value, label, detail;
        std::vector<int> hits;
    };
    using Source = std::function<void(std::string_view,
        std::function<void(std::vector<Item>)>)>;
    using Accept = std::function<void(const Item&, bool complete_only)>;

    Completion(tui::Runtime&, PromptBox&, std::function<void()> closed = {});
    ~Completion() override;
    void open(std::string title, Source, std::string hint, Accept = {});
    void refresh(std::string_view query);
    void close();
    bool visible() const noexcept { return overlay_ != 0; }
    void set_theme(const tui::ThemeTokens&);
    bool on_event(const tui::Event&) override;

private:
    class View;
    void accept(bool complete_only);

    tui::Runtime& rt_;
    PromptBox& prompt_;
    View* view_ = nullptr;
    uint32_t overlay_ = 0;
    std::uint64_t generation_ = 0;
    Source source_;
    Accept accept_;
    std::vector<Item> items_;
    int selected_ = 0;
    tui::ThemeTokens theme_ = tui::dark_theme();
    std::function<void()> closed_;
};

} // namespace dagent::ui
