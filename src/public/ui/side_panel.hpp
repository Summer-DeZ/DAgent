#pragma once
#include <map>
#include <functional>
#include "tui/input.hpp"
#include "ui/appearance.hpp"
#include "ui/projection.hpp"

namespace dagent::ui {
enum class AgentState { running, done, failed };
struct AgentInfo {
    std::string name;
    AgentState state = AgentState::running;
    double seconds = 0;
};
class SidePanel final : public tui::Widget, public tui::EventHandler {
public:
    struct Row {
        std::string left, right;
        std::function<void()> activate;
        bool current = false;
        char status = 0;
    };
    struct Section { std::string right; std::vector<Row> rows; };
    std::map<std::string, Section> sections;
    std::vector<std::string> order;
    SidePanel* source = nullptr;
    SidePanel* mirror = nullptr;
    void invalidate() noexcept { tui::Widget::invalidate(); if (mirror) mirror->tui::Widget::invalidate(); }
    bool left = false, focused = false;
    int columns = 30;
    std::function<void()> back, focus;
    std::function<void()> all_sessions;
    void set_focused(bool value) { focused = value; invalidate(); }
    bool on_event(const tui::Event&) override;
    bool dirty_tree() const noexcept override { return dirty_ || (source && source->dirty_tree()); }
    void set_agents(std::vector<AgentInfo> agents, int frame) { agents_ = std::move(agents); frame_ = frame; invalidate(); }
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
    std::vector<AgentInfo> agents_;
    int frame_ = 0;
    TodoList items_;
    std::vector<McpStatus> mcp_;
    std::map<std::string, std::pair<int, int>> changes_;
    std::filesystem::path root_;
    std::string title_, version_, started_;
    std::size_t used_ = 0, limit_ = 0;
    int turns_ = 0, trigger_ = 80;
    struct DisplayRow { Row row; std::string section; bool heading = false; };
    std::vector<DisplayRow> rows_;
    std::vector<int> selectable_;
    int selected_ = 0, top_ = 0;
    bool collapsed_ = false, outline_ = false, compact_ = false, has_plan_ = false;
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
};
} // namespace dagent::ui
