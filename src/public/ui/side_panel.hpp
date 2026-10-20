#pragma once
#include <map>
#include "ui/appearance.hpp"
#include "ui/projection.hpp"

namespace dagent::ui {
class SidePanel final : public tui::Widget {
public:
    void set_title(std::string);
    void set_context(std::size_t used, std::size_t limit);
    void set_items(TodoList);
    void set_mcp(std::vector<McpStatus>);
    void set_version(std::string);
    void set_collapsed(bool);
    void set_appearance(const Appearance&);
    void start_session(std::string_view id);
    void user_turn(const std::string&);
    void changed(const FileChangeView&);
    void set_project_root(std::filesystem::path root) { root_ = std::move(root); }
    void set_trigger(int value) { trigger_ = value; invalidate(); }
    [[nodiscard]] bool collapsed() const noexcept { return collapsed_; }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::pair<int, int> progress() const noexcept;
    const std::string& title() const { return title_; }
    void set_theme(const tui::ThemeTokens& theme) { theme_ = &theme; invalidate(); }
    tui::Size measure(tui::Size available) const override;
    void render(tui::Surface&) override;
private:
    TodoList items_;
    std::vector<McpStatus> mcp_;
    std::map<std::string, std::pair<int, int>> changes_;
    std::filesystem::path root_;
    std::string title_, version_, started_;
    std::size_t used_ = 0, limit_ = 0;
    int turns_ = 0, trigger_ = 80;
    bool collapsed_ = false, outline_ = false, compact_ = false, left_ = false, has_plan_ = false;
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
};
} // namespace dagent::ui
