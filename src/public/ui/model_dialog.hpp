#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "agent/provider.hpp"
#include "tui/runtime.hpp"

namespace dagent::ui {

/// @brief 从模型面板打开的逐步表单；只收集配置，不直接接触 models.json。
class ModelDialog final : public tui::EventHandler {
public:
    using Submit = std::function<void(agent::ProviderConfig)>;

    explicit ModelDialog(tui::Runtime&);
    ~ModelDialog() override;

    void open(std::size_t default_context_window, Submit);
    void close();
    bool active() const noexcept { return overlay_ != 0; }
    void set_theme(const tui::ThemeTokens&);
    bool on_event(const tui::Event&) override;

private:
    class View;
    bool accept_field();
    void move(int delta);
    void refresh();
    void fail(std::string message);

    tui::Runtime& rt_;
    View* view_ = nullptr;
    uint32_t overlay_ = 0;
    std::vector<std::string> values_;
    int step_ = 0;
    Submit submit_;
    tui::ThemeTokens theme_ = tui::dark_theme();
};

} // namespace dagent::ui
