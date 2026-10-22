#include "ui/shell_detail.hpp"

#include <cstdlib>
#include <format>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dagent::ui {

namespace {
using namespace std::chrono_literals;
} // namespace

void Shell::reset_transcript(bool resumed, std::size_t messages) {
    close_child_panes();
    active_transcript().clear();
    side_->start_session(id_); header_->title.clear(); header_->invalidate();
    if (resumed) active_transcript().resumed(id_, messages, std::string(ui::text().panel_just_now));
    // 新会话也有 system 历史记录；历史条目数不能用来判断是否已发送消息。
    frame_->welcome.visible = !resumed;
    input_->set_welcome(!resumed);
    frame_->welcome.selected = -1; frame_->welcome.top = 0; frame_->welcome.sessions.clear(); frame_->welcome.memories = 0;
    update_welcome(); frame_->invalidate_tree();
    if (frame_->welcome.visible) refresh_welcome();
    update_todo_status(terminal_.size().cols);
}

void Shell::refresh_agents() {
    std::vector<AgentInfo> agents, tabs;
    std::set<std::string> seen;
    for (size_t i = 0; i < panes_.size(); ++i) {
        const auto& pane = panes_[i];
        const auto state = i == 0 && busy_ ? AgentState::running : pane.state;
        const auto seconds = state == AgentState::running ? std::chrono::duration<double>(Clock::now() - pane.began).count() : pane.seconds;
        const auto name = i == 0 ? pane.title : pane.title.substr(pane.title.find(" · ") + 4);
        tabs.push_back({name, state, seconds});
        if (i != 0) { agents.push_back({name, state, seconds}); seen.insert(pane.call_id); }
    }
    if (!panes_.empty()) for (const auto& task : panes_[0].transcript->tasks()) {
        if (seen.insert(task.call_id).second) agents.push_back({task.agent, task.failed ? AgentState::failed : AgentState::done, task.seconds});
    }
    const bool resized = tabs_->tabs.size() != tabs.size();
    tabs_->tabs = std::move(tabs); tabs_->active = active_; tabs_->frame = animation_;
    if (resized) tabs_->invalidate_layout(); else tabs_->invalidate();
    side_->set_agents(std::move(agents), animation_);
}

RenderOptions Shell::render_options() const {
    return {.auto_diff = appearance_.diff == "auto",
            .unified = unified_diff_,
            .links = appearance_.links == "on",
            .project = project_root_};
}

std::size_t Shell::add_pane(std::string title, std::string session_id, std::string call_id) {
    tui::Scrollback& scroll = frame_->create();
    const std::size_t index = panes_.size();
    auto transcript = std::make_unique<Transcript>(
        scroll.document(), [this, index](const TodoList& value) {
            if (index == 0) update_todo(value);
        }, [this, index](const FileChangeView& value) {
            if (index == 0) side_->changed(value);
        }, [this, index](const std::string& input) {
            if (index != 0) return;
            side_->user_turn(input); header_->title = side_->title(); header_->invalidate();
        });
    transcript->set_render_options(render_options());
    transcript->set_compact(appearance_.density == "compact");
    transcript->set_session(mode_label(), model_label_);
    scroll.set_theme(theme_);
    auto mouse = std::make_unique<tui::ScrollbackMouse>(rt_, scroll);
    panes_.push_back({&scroll, std::move(transcript), std::move(mouse), {}, std::move(title),
                      std::move(session_id), std::move(call_id)});
    if (index == 0) panes_[0].state = AgentState::done;
    refresh_agents();
    return index;
}

void Shell::show_pane(std::size_t index) {
    if (index == active_ || index >= panes_.size()) return;
    unbind_active_mouse();
    active_ = index;
    frame_->show(active_scroll());
    active_scroll().set_theme(theme_);
    bind_active_mouse();
    input_->set_active(!busy_ && active_ == 0);
    update_prompt_footer();
    refresh_agents();
    root_.invalidate_tree();
}

void Shell::close_child_panes() {
    if (active_ != 0) {
        unbind_active_mouse();
        active_ = 0;
        frame_->show(active_scroll());
        bind_active_mouse();
    }
    for (std::size_t i = 1; i < panes_.size(); ++i) {
        if (panes_[i].pager) panes_[i].pager->cancel();
        panes_[i].transcript.reset();
        panes_[i].mouse.reset();
        frame_->destroy(*panes_[i].scroll);
    }
    panes_.resize(1);
    task_panes_.clear(); submitted_.clear();
    refresh_agents();
    input_->set_active(!busy_);
}

void Shell::open_task_pane(const Transcript::TaskRef& task) {
    const std::size_t index = add_pane("task · " + task.agent, task.session_id, task.call_id);
    panes_[index].state = task.failed ? AgentState::failed : AgentState::done;
    panes_[index].seconds = task.seconds;
    panes_[index].description = task.description;
    task_panes_[task.call_id] = index;
    show_pane(index);
    const std::string id = task.session_id;
    auto pager = std::make_shared<HistoryPager>(client_, rt_, gate_, id);
    pager->on_item = [this, index, id](const protocol::HistoryItem& item) {
        if (index >= panes_.size() || panes_[index].session_id != id) return;
        panes_[index].transcript->append_history(item);
    };
    pager->on_error = [this, index, id](std::string message) {
        if (index >= panes_.size() || panes_[index].session_id != id) return;
        toast(std::string(ui::text().toast_resume_failed) + message, tui::Notice::Severity::error);
    };
    pager->on_done = [this, index, id] {
        if (index >= panes_.size() || panes_[index].session_id != id) return;
        root_.invalidate_tree();
    };
    panes_[index].pager = pager;
    pager->start();
}

void Shell::agent_panel() {
    std::vector<Panel::Row> rows;
    rows.push_back({"main", id_.substr(0, 8), active_ == 0 ? std::string(ui::text().panel_current) : "",
                    true, [this] { show_pane(0); }});
    std::set<std::string> seen;
    std::size_t no = 0;
    for (std::size_t i = 1; i < panes_.size(); ++i) {
        seen.insert(panes_[i].session_id);
        rows.push_back({std::format("{}. {}", ++no, panes_[i].title),
                        panes_[i].description,
                        active_ == i ? std::string(ui::text().panel_current) : "", true,
                        [this, i] { show_pane(i); }});
    }
    for (const Transcript::TaskRef& task : panes_[0].transcript->tasks()) {
        if (!seen.insert(task.session_id).second) continue;
        rows.push_back({std::format("{}. task · {}", ++no, task.agent),
                        task.description, "", true,
                        [this, task] { open_task_pane(task); }});
    }
    panel_.open(std::string(ui::text().panel_agents), std::move(rows),
                std::string(ui::text().panel_agent_footer));
}

} // namespace dagent::ui
