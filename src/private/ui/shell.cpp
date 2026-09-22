#include "ui/shell.hpp"

#include "ui/strings.hpp"
#include "ui/display.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <format>
#include <mutex>
#include <functional>
#include <iostream>
#include <set>
#include <utility>
#include <variant>

#include "base/log.hpp"
#include "ui/approval.hpp"
#include "ui/center.hpp"
#include "ui/completion.hpp"
#include "ui/model_dialog.hpp"
#include "ui/panel.hpp"
#include "ui/prompt_box.hpp"
#include "ui/prompt_input.hpp"
#include "ui/status_line.hpp"
#include "ui/theme_config.hpp"
#include "ui/toast.hpp"
#include "ui/side_panel.hpp"
#include "ui/transcript.hpp"

#ifndef DAGENT_VERSION
#define DAGENT_VERSION "dev"
#endif

namespace dagent::ui {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
template<class... T> struct Overloaded : T... { using T::operator()...; };

class QueueLabel final : public tui::Text {
public:
    tui::Size measure(tui::Size available) const override {
        return text().empty() ? tui::Size{} : tui::Text::measure(available);
    }
};

class ScrollFrame final : public tui::Widget {
public:
    ScrollFrame() = default;
    ~ScrollFrame() override { if (current_ != nullptr) disown(*current_); }

    /// 新建一个滚动区；第一个自动成为当前显示的那个。
    tui::Scrollback& create() {
        auto scroll = std::make_unique<tui::Scrollback>();
        tui::Scrollback* pointer = scroll.get();
        scrolls_.push_back(std::move(scroll));
        if (current_ == nullptr) show(*pointer);
        return *pointer;
    }
    /// 切换显示的滚动区；旧的 disown、新的 adopt。
    void show(tui::Scrollback& scroll) {
        if (current_ == &scroll) return;
        if (current_ != nullptr) disown(*current_);
        current_ = &scroll;
        adopt(*this, scroll);
        invalidate_tree();
    }
    /// 删除一个滚动区；调用方必须先解绑它的鼠标处理器。
    void destroy(tui::Scrollback& scroll) {
        if (current_ == &scroll) {
            disown(scroll);
            current_ = nullptr;
        }
        std::erase_if(scrolls_, [&](const std::unique_ptr<tui::Scrollback>& item) {
            return item.get() == &scroll;
        });
    }
    tui::Scrollback* current() const { return current_; }

    tui::Size measure(tui::Size available) const override { return available; }
    void layout(tui::Rect area) override {
        tui::Widget::layout(area);
        if (current_ != nullptr) current_->layout({0, 0, area.w, area.h});
    }
    void render(tui::Surface& surface) override {
        if (current_ == nullptr) return;
        if (current_->dirty_tree()) {
            current_->render(surface); current_->clear_dirty();
        }
        if (!current_->pinned() && current_->unseen_rows() > 0 && surface.rows() > 0) {
            const std::string text = format_text(ui::text().card_unseen, current_->unseen_rows());
            const int x = std::max(0, surface.cols() - display_width(text));
            surface.text(x, surface.rows() - 1, text, current_->theme().text_muted);
        }
        clear_dirty();
    }
    bool dirty_tree() const noexcept override {
        return dirty_ || (current_ != nullptr && current_->dirty_tree());
    }
    bool needs_layout() const noexcept override {
        return layout_dirty_ || (current_ != nullptr && current_->needs_layout());
    }
    void invalidate_tree() noexcept override {
        invalidate();
        if (current_ != nullptr) current_->invalidate_tree();
    }
    void invalidate_rect(tui::Rect rect) override {
        if (current_ != nullptr) current_->invalidate_rect(rect);
        Widget::invalidate_rect(rect);
    }
    tui::Widget* hit_test(tui::Point point) const noexcept override {
        if (current_ != nullptr) {
            if (auto* hit = current_->hit_test(point)) return hit;
        }
        return Widget::hit_test(point);
    }
private:
    std::vector<std::unique_ptr<tui::Scrollback>> scrolls_;
    tui::Scrollback* current_ = nullptr;
};

std::string display_path(const std::filesystem::path& path) {
    const std::string value = path.string();
    if (const char* home = std::getenv("HOME")) {
        const std::string prefix(home);
        if (value == prefix) return "~";
        if (value.starts_with(prefix + "/")) return "~" + value.substr(prefix.size());
    }
    return value;
}

std::string relative_age(std::chrono::system_clock::time_point time) {
    const auto elapsed = std::chrono::system_clock::now() - time;
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(elapsed).count();
    if (minutes < 1) return std::string(ui::text().panel_just_now);
    if (minutes < 60) return format_text(ui::text().panel_minutes, minutes);
    const auto hours = minutes / 60;
    if (hours < 24) return format_text(ui::text().panel_hours, hours);
    return format_text(ui::text().panel_days, hours / 24);
}

std::vector<int> subsequence_hits(std::string_view needle, std::string_view value) {
    std::vector<int> hits;
    std::size_t from = 0;
    for (char raw : needle) {
        const char target = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
        while (from < value.size() &&
               static_cast<char>(std::tolower(static_cast<unsigned char>(value[from]))) != target) ++from;
        if (from == value.size()) break;
        hits.push_back(static_cast<int>(from++));
    }
    return hits;
}

/// @brief 前端投影：只持 Runtime、只读 DTO 与页面状态，不持 Agent/Setup，不写业务状态（R07）。
class Shell final : public tui::EventHandler, public runtime::Frontend {
public:
    Shell(runtime::Runtime& runtime, const runtime::RuntimeSnapshot& initial,
          runtime::StartResult started, std::vector<agent::Event> history,
          std::optional<ThemeSet> themes, const InteractiveOptions& options)
        : runtime_(runtime), themes_(std::move(themes)), resumed_(started.resumed),
          replay_(std::move(started.replay)), history_(std::move(history)),
          initial_prompt_(options.initial_prompt), root_(layout()), rt_(terminal_, root_),
          prompt_(*input_, [this](std::string text) { submit(std::move(text)); },
                  [this] { recall(); }, [this] { prompt_changed(); },
                  [this] { return completion_.visible(); }),
          keys_(rt_), dialog_(rt_, [this] { interrupt(); }), model_dialog_(rt_),
          toasts_(rt_), panel_(rt_),
          completion_(rt_, *input_, [this] { completion_kind_.clear(); }) {
        apply_snapshot(initial);
        project_path_ = display_path(project_root_);
        status_->project(project_path_);
        status_->set_trigger(trigger_percent_);
        side_->set_project(project_path_, {});
        side_->set_version(DAGENT_VERSION);
        add_pane("main", id_, {});
        update_prompt_footer();
        rt_.set_focus(&prompt_, input_); rt_.set_global(*this);
        terminal_.set_mouse(true); // 不开这一行，滚轮与拖选的转义序列根本不会上报
        bind_active_mouse();
        register_commands();
        apply_theme(theme_for(theme_choice_));
        rt_.on_caps([this](const tui::Terminal::Caps&) {
            if (theme_choice_ == "follow" && !theme_preview_) apply_theme(theme_for(theme_choice_));
        });
        refresh_models();
        reset_transcript(resumed_, replay_.size() + history_.size());
        for (const auto& event : history_) apply_core(event);
        for (const auto& event : replay_) apply_core(event);
        refresh_project();
        refresh_mcp();
    }

    ~Shell() override {
        rt_.cancel(file_debounce_);
        terminal_.restore();
        unbind_active_mouse();
    }

    int run(agent::Interrupts& interrupts) {
        interrupts.graceful = true;
        std::stop_callback signal(interrupts.stop.get_token(), [this] {
            rt_.post([this] { signal_exit_ = true; exit(); });
        });
        if (!initial_prompt_.empty()) submit(initial_prompt_);
        rt_.run();
        terminal_.restore();
        runtime_.shutdown();
        interrupts.graceful = false;
        if (!error_.empty()) std::cerr << "failed: " << error_ << '\n';
        std::cout << std::format("Session {0} saved. Resume with: dagent -r {0}\n", id_);
        return !error_.empty() ? 1 : signal_exit_ ? 130 : 0;
    }

    bool on_event(const tui::Event& event) override {
        if (event.kind == tui::Event::Kind::resize) {
            active_transcript().set_todo_narrow(event.size.cols < 80);
            update_todo_status(event.size.cols);
        }
        return keys_.on_event(event);
    }

private:
    struct CommandUi { std::string id, shortcut, alias; };

    // ---- runtime::Frontend：后端线程 → 渲染线程 ----
    void event(const runtime::Event& event) override {
        rt_.post([this, event] { if (!exiting_) apply(event); });
    }
    void interaction_requested(const runtime::InteractionRequest& request) override {
        rt_.post([this, request] { if (!exiting_) show_interaction(request); });
    }
    void interaction_closed(const std::string& interaction_id) override {
        rt_.post([this, interaction_id] {
            if (exiting_) return;
            if (active_interaction_ == interaction_id) {
                active_interaction_.clear();
                dialog_.close();
            }
        });
    }

    void apply_snapshot(const runtime::RuntimeSnapshot& snapshot) {
        id_ = snapshot.session_id;
        model_name_ = snapshot.model.name;
        model_label_ = snapshot.model.model;
        planning_ = snapshot.planning;
        mode_ = snapshot.permission_mode;
        read_only_ = snapshot.read_only;
        worked_tokens_ = snapshot.used_tokens;
        token_limit_ = snapshot.token_limit;
        trigger_percent_ = snapshot.trigger_percent;
        window_tokens_ = snapshot.window_tokens;
        project_root_ = snapshot.project_root;
        if (!snapshot.project_root.empty()) project_path_ = display_path(snapshot.project_root);
        mcp_states_ = snapshot.mcp;
        if (!snapshot.session_id.empty()) {
            const auto [done, total] = side_->progress();
            status_->todo(done, total, total > 0 && (side_->collapsed() || terminal_.size().cols < 80));
        }
    }

    std::unique_ptr<tui::Widget> layout() {
        auto main = std::make_unique<tui::Container>();
        auto add_centered = [&]<class T>(T*& target, tui::Constraint constraint) {
            auto widget = std::make_unique<T>(); target = widget.get();
            main->add(constraint, centered(std::move(widget), 88, theme_));
        };
        auto frame = std::make_unique<ScrollFrame>(); frame_ = frame.get();
        main->add({tui::Sizing::flex, 1}, centered(std::move(frame), 88, theme_));
        add_centered(activity_, {tui::Sizing::content});
        add_centered(queue_label_, {tui::Sizing::content, 0, 0, 3});
        add_centered(input_, {tui::Sizing::content, 0, 4, PromptBox::k_max_rows + 3});

        auto workspace = std::make_unique<tui::Container>(tui::Container::Direction::horizontal);
        workspace->add({tui::Sizing::flex, 1}, std::move(main));
        auto side = std::make_unique<SidePanel>(); side_ = side.get();
        workspace->add({tui::Sizing::content}, std::move(side));

        auto page = std::make_unique<tui::Container>();
        page->add({tui::Sizing::flex, 1}, std::move(workspace));
        auto status = std::make_unique<StatusLine>(); status_ = status.get();
        page->add({tui::Sizing::fixed, 1}, std::move(status));
        return page;
    }

    void add_command(std::string id, std::string title, std::string category,
                     std::string shortcut, std::string alias, std::function<void()> fn,
                     std::function<bool()> enabled = {}) {
        keys_.add({id, std::move(title), std::move(category), std::move(fn), std::move(enabled)});
        command_ui_.push_back({id, shortcut, alias});
        if (!shortcut.empty() && !keys_.bind(shortcut, id))
            throw std::logic_error("invalid key binding: " + shortcut);
    }
    void register_commands() {
        add_command("panel.commands", std::string(ui::text().cmd_palette), std::string(ui::text().cmd_view), "ctrl+p", {}, [this] { command_panel(); });
        add_command("session.interrupt", std::string(ui::text().cmd_interrupt), std::string(ui::text().cmd_session), "escape", {}, [this] { interrupt(); });
        add_command("session.cancel", std::string(ui::text().cmd_cancel), std::string(ui::text().cmd_session), "ctrl+c", {}, [this] { cancel(); });
        add_command("permission.cycle", std::string(ui::text().cmd_permission), std::string(ui::text().cmd_permission_group), "shift+tab", {}, [this] { cycle_permission(); });
        add_command("permission.list", "Session permissions", std::string(ui::text().cmd_permission_group), {}, "/permissions", [this] { permissions_panel(); }, [this] { return !busy_; });
        add_command("mode.plan", "Toggle planning mode", std::string(ui::text().cmd_permission_group), "ctrl+g", "/plan", [this] { toggle_plan(); });
        add_command("tools.expand", std::string(ui::text().cmd_tools), std::string(ui::text().cmd_transcript), "ctrl+o", {}, [this] { active_transcript().toggle_tools(); });
        add_command("thoughts.toggle", std::string(ui::text().cmd_thoughts), std::string(ui::text().cmd_transcript), "ctrl+r", {}, [this] { active_transcript().toggle_thoughts(); });
        add_command("todo.toggle", std::string(ui::text().cmd_todo), std::string(ui::text().cmd_view), "ctrl+t", {}, [this] { toggle_todo(); });
        add_command("scroll.up", std::string(ui::text().cmd_page_up), std::string(ui::text().cmd_transcript), "pageup", {}, [this] { active_scroll().scroll_pages(-1); });
        add_command("scroll.down", std::string(ui::text().cmd_page_down), std::string(ui::text().cmd_transcript), "pagedown", {}, [this] { active_scroll().scroll_pages(1); });
        add_command("scroll.home", std::string(ui::text().cmd_home), std::string(ui::text().cmd_transcript), "home", {}, [this] { active_scroll().scroll_home(); });
        add_command("scroll.end", std::string(ui::text().cmd_end), std::string(ui::text().cmd_transcript), "end", {}, [this] { active_scroll().scroll_end(); });
        add_command("agent.switch", std::string(ui::text().cmd_agents), std::string(ui::text().cmd_view), "ctrl+a", "/agents", [this] { agent_panel(); });
        add_command("session.new", std::string(ui::text().cmd_new), std::string(ui::text().cmd_session), {}, "/new", [this] { new_session(); });
        add_command("session.compact", std::string(ui::text().cmd_compact), std::string(ui::text().cmd_session), {}, "/compact", [this] { compact(); });
        add_command("session.list", std::string(ui::text().cmd_sessions), std::string(ui::text().cmd_session), {}, "/sessions", [this] { session_panel(); });
        add_command("model.pick", std::string(ui::text().cmd_model), std::string(ui::text().cmd_session), "ctrl+m", "/model", [this] { model_panel(); }, [this] { return !busy_; });
        add_command("theme.pick", std::string(ui::text().cmd_theme), std::string(ui::text().cmd_view), {}, "/theme", [this] { theme_panel(); });
        add_command("help.show", std::string(ui::text().cmd_help), std::string(ui::text().cmd_view), "ctrl+?", "/help", [this] { help_panel(); });
        add_command("session.exit", std::string(ui::text().cmd_exit), std::string(ui::text().cmd_session), {}, "/exit", [this] { exit(); });
    }
    void run_command(std::string_view id) {
        const auto it = std::ranges::find_if(keys_.commands(), [&](const tui::Command& command) {
            return command.id == id;
        });
        if (it != keys_.commands().end() && (!it->enabled || it->enabled())) it->run();
    }

    void reset_transcript(bool resumed = false, std::size_t messages = 0) {
        close_child_panes();
        active_transcript().clear();
        if (resumed) active_transcript().resumed(id_, messages, std::string(ui::text().panel_just_now));
        else active_transcript().banner(DAGENT_VERSION, project_path_, branch_,
                                        static_cast<int>(mcp_states_.size()));
        active_transcript().set_todo_narrow(terminal_.size().cols < 80);
    }

    // ---- 子会话 Pane：索引 0 恒为主会话，task 的子 Agent 各占一个 Pane ----
    struct Pane {
        tui::Scrollback* scroll = nullptr;
        std::unique_ptr<Transcript> transcript;
        std::unique_ptr<tui::ScrollbackMouse> mouse;
        std::string title, session_id, call_id;
    };
    tui::Scrollback& active_scroll() { return *panes_[active_].scroll; }
    Transcript& active_transcript() { return *panes_[active_].transcript; }
    void bind_active_mouse() { rt_.bind_mouse(active_scroll(), *panes_[active_].mouse); }
    void unbind_active_mouse() { rt_.unbind_mouse(active_scroll()); }

    std::size_t add_pane(std::string title, std::string session_id, std::string call_id) {
        tui::Scrollback& scroll = frame_->create();
        const std::size_t index = panes_.size();
        auto transcript = std::make_unique<Transcript>(
            scroll.document(), [this, index](const agent::TodoView& value) {
                if (index == 0) update_todo(value);
            });
        transcript->set_session(mode_label(), model_label_);
        scroll.set_theme(theme_);
        auto mouse = std::make_unique<tui::ScrollbackMouse>(rt_, scroll);
        panes_.push_back({&scroll, std::move(transcript), std::move(mouse), std::move(title),
                          std::move(session_id), std::move(call_id)});
        return index;
    }
    void show_pane(std::size_t index) {
        if (index == active_ || index >= panes_.size()) return;
        unbind_active_mouse();
        active_ = index;
        frame_->show(active_scroll());
        active_scroll().set_theme(theme_);
        bind_active_mouse();
        input_->set_active(!busy_ && active_ == 0);
        update_prompt_footer();
        root_.invalidate_tree();
    }
    void close_child_panes() {
        if (active_ != 0) {
            unbind_active_mouse();
            active_ = 0;
            frame_->show(active_scroll());
            bind_active_mouse();
        }
        for (std::size_t i = 1; i < panes_.size(); ++i) {
            panes_[i].transcript.reset();
            panes_[i].mouse.reset();
            frame_->destroy(*panes_[i].scroll);
        }
        panes_.resize(1);
        task_panes_.clear();
        input_->set_active(!busy_);
    }
    /// 恢复出来的 task 没有 Pane：切进去时再回放子会话记录。
    void open_task_pane(const Transcript::TaskRef& task) {
        const std::size_t index = add_pane("task · " + task.agent, task.session_id, task.call_id);
        show_pane(index);
        runtime_.query_history(task.session_id,
                               [this, index, id = task.session_id](runtime::Runtime::HistoryResult result) {
            if (!result) {
                rt_.post([this, message = result.error().message] {
                    toast(std::string(ui::text().toast_resume_failed) + message,
                          tui::Notice::Severity::error);
                });
                return;
            }
            rt_.post([this, index, id, events = std::move(*result)]() mutable {
                if (index >= panes_.size() || panes_[index].session_id != id) return;
                for (const auto& event : events) panes_[index].transcript->apply(event);
                root_.invalidate_tree();
            });
        });
    }
    void agent_panel() {
        std::vector<Panel::Row> rows;
        rows.push_back({"main", id_.substr(0, 8), active_ == 0 ? std::string(ui::text().panel_current) : "",
                        true, [this] { show_pane(0); }});
        std::set<std::string> seen;
        std::size_t no = 0;
        for (std::size_t i = 1; i < panes_.size(); ++i) {
            seen.insert(panes_[i].session_id);
            rows.push_back({std::format("{}. {}", ++no, panes_[i].title),
                            panes_[i].session_id.substr(0, 8),
                            active_ == i ? std::string(ui::text().panel_current) : "", true,
                            [this, i] { show_pane(i); }});
        }
        for (const Transcript::TaskRef& task : panes_[0].transcript->tasks()) {
            if (!seen.insert(task.session_id).second) continue;
            rows.push_back({std::format("{}. task · {}", ++no, task.agent),
                            task.session_id.substr(0, 8), "", true,
                            [this, task] { open_task_pane(task); }});
        }
        panel_.open(std::string(ui::text().panel_agents), std::move(rows),
                    std::string(ui::text().panel_agent_footer));
    }
    void refresh_project() {
        runtime_.query_workspace([this](runtime::Runtime::WorkspaceResult result) {
            if (!result) return;
            std::string branch = result->branch;
            rt_.post([this, branch = std::move(branch)] {
                branch_ = branch; side_->set_project(project_path_, branch_);
            });
        });
    }
    void toast(std::string text, tui::Notice::Severity severity = tui::Notice::Severity::info) {
        toasts_.show(severity, std::move(text));
    }
    void apply_theme(const tui::ThemeTokens& selected) {
        theme_ = resolve_theme(selected); theme_.epoch = ++theme_epoch_;
        for (Pane& pane : panes_) pane.scroll->set_theme(theme_);
        activity_->set_theme(theme_);
        queue_label_->set_theme(theme_); queue_label_->set_style(theme_.text_muted);
        input_->set_theme(theme_); status_->set_theme(theme_);
        side_->set_theme(theme_); toasts_.set_theme(theme_); panel_.set_theme(theme_);
        dialog_.set_theme(theme_); model_dialog_.set_theme(theme_); completion_.set_theme(theme_);
        root_.invalidate_tree();
    }
    void activity() {
        if (!busy_) { activity_->set_action({}); return; }
        if (dialog_.active()) { activity_->set_action(std::string(ui::text().act_waiting)); return; }
        std::string label = phase_;
        auto begin = step_begin_;
        if (!running_.empty()) { label = std::string(ui::text().act_running) + running_.front().summary; begin = running_.front().begin; }
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - begin).count();
        activity_->set_action(format_text(ui::text().act_line, label, seconds));
    }
    bool update_mcp() {
        mcp_states_ = runtime_.snapshot().mcp;
        side_->set_mcp(mcp_label(mcp_states_));
        for (const auto& state : mcp_states_) {
            using Status = agent::McpServerState::Status;
            if (state.status == Status::connecting || state.status == Status::reconnecting) return true;
        }
        return false;
    }
    void refresh_mcp() {
        const bool pending = update_mcp();
        if (!pending && !busy_) return;
        if (mcp_timer_) return;
        mcp_timer_ = rt_.every(200ms, [this] {
            const bool pending_now = update_mcp();
            if (pending_now || busy_) return true;
            mcp_timer_ = 0; return false;
        });
    }
    void busy(bool value) {
        busy_ = value; rt_.cancel(activity_timer_); activity_timer_ = 0;
        input_->set_active(!value && active_ == 0); update_prompt_footer();
        if (value) {
            step_begin_ = Clock::now(); phase_ = std::string(ui::text().act_thinking);
            activity_timer_ = rt_.every(100ms, [this] { activity(); activity_->tick(); return true; });
        } else running_.clear();
        refresh_mcp(); activity();
    }
    void apply(const runtime::Event& event) {
        std::visit(Overloaded{
                       [&](const agent::Event& core) { apply_core(core); },
                       [&](const runtime::ControlEvent& control) { apply_control(control); },
                   },
                   event.payload);
        activity();
    }
    void apply_control(const runtime::ControlEvent& event) {
        switch (event.kind) {
        case runtime::ControlEvent::Kind::session_replaced: {
            const runtime::RuntimeSnapshot snapshot = runtime_.snapshot();
            apply_snapshot(snapshot);
            if (event.replace_transcript) {
                reset_transcript(event.resumed, 0);
                side_->set_title({});
                if (event.resumed) refresh_history();
            } else {
                active_transcript().set_session(mode_label(), model_label_);
                update_prompt_footer();
            }
            busy(false);
            refresh_project();
            refresh_models();
            break;
        }
        case runtime::ControlEvent::Kind::models_changed:
            refresh_models();
            toast(std::string(ui::text().toast_model_added) + runtime_.snapshot().model.name);
            break;
        case runtime::ControlEvent::Kind::operation_finished:
            if (event.operation == "compact") {
                if (event.status == agent::TurnStatus::interrupted)
                    toast(std::string(ui::text().toast_compact_cancelled));
                busy(false);
            }
            break;
        case runtime::ControlEvent::Kind::failed:
            toast(event.operation + " failed: " + event.error, tui::Notice::Severity::error);
            busy(false);
            break;
        }
    }
    void apply_core(const agent::Event& event) {
        if (!std::holds_alternative<agent::SubEvent>(event)) active_transcript().apply(event);
        std::visit(Overloaded{
            [&](const agent::TurnStarted&) { busy(true); refresh_queue(); },
            [&](const agent::StepStarted&) { phase_ = std::string(ui::text().act_thinking); step_begin_ = Clock::now(); },
            [&](const agent::TextDelta&) { phase_ = std::string(ui::text().act_generating); },
            [&](const agent::ReasoningDelta&) { phase_ = std::string(ui::text().act_generating); },
            [&](const agent::ToolPending& e) { phase_ = std::string(ui::text().act_preparing) + e.name; },
            [&](const agent::ToolStarted& e) { running_.push_back({e.id, e.summary, Clock::now()}); },
            [&](const agent::SubEvent& e) {
                phase_ = format_text(ui::text().act_task, e.agent);
                std::size_t index = 0;
                if (const auto it = task_panes_.find(e.call_id); it != task_panes_.end()) {
                    index = it->second;
                } else {
                    index = add_pane("task · " + e.agent, e.session, e.call_id);
                    task_panes_.emplace(e.call_id, index);
                }
                if (index != 0) panes_[index].transcript->apply(e.event()); // 子 Pane 自己的记录
            },
            [&](const agent::ToolFinished& e) {
                std::erase_if(running_, [&](const Running& running) { return running.id == e.id; });
            },
            [&](const agent::ContextUpdate& e) { status_->context(e); side_->set_context(e.used, e.limit); },
            [&](const agent::Retrying& e) {
                toast(format_text(ui::text().toast_retry, e.reason, e.wait.count() / 1000.0,
                                  e.attempt, e.max_attempts), tui::Notice::Severity::warn);
            },
            [&](const agent::Notice& e) {
                if (e.level != agent::Notice::Level::error)
                    toast(e.text, e.level == agent::Notice::Level::warn
                                      ? tui::Notice::Severity::warn : tui::Notice::Severity::info);
            },
            [&](const agent::ModeChanged& e) {
                planning_ = e.planning;
                mode_ = e.mode == "ask" ? agent::PermissionMode::ask
                      : e.mode == "unrestricted" ? agent::PermissionMode::unrestricted
                                                   : agent::PermissionMode::workspace;
                active_transcript().set_session(mode_label(), model_label_);
                update_prompt_footer();
            },
            [&](const agent::TurnEnded&) { dialog_.close(); busy(false); refresh_queue(); },
            [](const auto&) {}
        }, event);
    }

    void refresh_history() {
        runtime_.query_history(id_, [this, id = id_](runtime::Runtime::HistoryResult result) {
            if (!result) return;
            rt_.post([this, id, events = std::move(*result)]() mutable {
                if (id_ != id || exiting_) return;
                reset_transcript(true, events.size());
                for (const auto& event : events) apply_core(event);
                rt_.post([this] { refresh_mcp(); });
            });
        });
    }
    void refresh_models() {
        models_ = runtime_.models();
        provider_kinds_ = runtime_.provider_kinds();
    }
    void show_interaction(const runtime::InteractionRequest& request) {
        active_interaction_ = request.id;
        if (request.kind == runtime::InteractionRequest::Kind::approval) {
            dialog_.open(request.approval, [this, id = request.id](agent::Decision decision) {
                active_interaction_.clear();
                runtime_.answer(id, std::move(decision));
            });
        } else {
            dialog_.open(request.question, [this, id = request.id](agent::Answer answer) {
                active_interaction_.clear();
                runtime_.answer(id, std::move(answer));
            });
        }
    }

    void update_todo(const agent::TodoView& value) {
        side_->set_items(value.items);
        update_todo_status(terminal_.size().cols);
    }
    void update_todo_status(int columns) {
        const auto [done, total] = side_->progress();
        status_->todo(done, total, total > 0 && (side_->collapsed() || columns < 80));
    }
    void toggle_todo() {
        side_->set_collapsed(!side_->collapsed());
        active_transcript().set_todo_collapsed(side_->collapsed());
        update_todo_status(terminal_.size().cols);
    }
    /// 输入框尾行与消息尾行共用同一份「模式 · 模型」。
    std::string mode_label() const {
        if (planning_) return "plan";
        if (mode_ == agent::PermissionMode::unrestricted) return "unrestricted";
        return std::string(mode_ == agent::PermissionMode::workspace ? ui::text().status_auto_edit
                                                                     : ui::text().status_ask);
    }
    void update_prompt_footer() {
        input_->set_footer(format_text(ui::text().box_footer, mode_label(), model_label_));
        input_->set_footer_tone(!planning_ && mode_ == agent::PermissionMode::unrestricted, planning_);
    }
    static std::string mcp_label(const std::vector<agent::McpServerState>& states) {
        if (states.empty()) return {};
        const auto ready = std::ranges::count_if(states, [](const agent::McpServerState& state) {
            return state.status == agent::McpServerState::Status::ready;
        });
        return format_text(ui::text().status_mcp, ready, states.size());
    }

    void refresh_queue() {
        const runtime::RuntimeSnapshot snapshot = runtime_.snapshot();
        std::string label;
        if (!snapshot.queue.empty()) {
            label = format_text(ui::text().queue_count, snapshot.queue.size());
            const std::size_t first = snapshot.queue.size() > 2 ? snapshot.queue.size() - 2 : 0;
            for (std::size_t i = first; i < snapshot.queue.size(); ++i)
                label += std::format("\n{} {}", i - first + 1,
                                     snapshot.queue[i].text.substr(0, snapshot.queue[i].text.find('\n')));
        }
        queue_label_->set_text(std::move(label));
    }
    void recall() {
        if (const auto recalled = runtime_.recall_last()) {
            input_->set_text(recalled->text);
            refresh_queue(); prompt_changed();
        }
    }
    void submit(std::string text) {
        if (exiting_) return;
        // 命令立即执行，不进待发队列：能否在忙碌时执行由各命令自己的 enabled 决定。
        if (text.starts_with('/')) {
            if (busy_ && text == "/model") {
                toast(std::string(ui::text().toast_model_busy), tui::Notice::Severity::warn);
                return;
            }
            command(text);
            return;
        }
        // 子视图是子会话的只读投影，消息属于主会话。
        if (active_ != 0) {
            input_->set_text(std::move(text)); // 保留已输入的内容，不让用户白打一遍
            toast(std::string(ui::text().toast_subview_readonly), tui::Notice::Severity::warn);
            return;
        }
        set_session_title(text);
        if (auto accepted = runtime_.submit(std::move(text)); !accepted) {
            toast(accepted.error().message, tui::Notice::Severity::warn);
        }
        refresh_queue();
    }
    /// 侧栏标题取本会话第一条真实输入的首行。
    void set_session_title(const std::string& input) {
        if (titled_) return;
        const std::string line = input.substr(0, input.find('\n'));
        if (line.empty()) return;
        side_->set_title(fit_columns(line, 40)); titled_ = true;
    }
    void command(const std::string& text) {
        const auto ui = std::ranges::find_if(command_ui_, [&](const CommandUi& item) { return item.alias == text; });
        if (ui == command_ui_.end()) toast(std::string(ui::text().toast_unknown_command) + text, tui::Notice::Severity::warn);
        else run_command(ui->id);
    }
    void compact() {
        if (busy_) return;
        if (auto accepted = runtime_.compact(); !accepted) {
            toast(accepted.error().message, tui::Notice::Severity::warn);
            return;
        }
        busy(true); phase_ = std::string(ui::text().act_compacting); activity();
    }
    void new_session() {
        if (busy_) return;
        if (auto accepted = runtime_.new_session(); !accepted) {
            toast(accepted.error().message, tui::Notice::Severity::warn);
            return;
        }
        busy(true); phase_ = std::string(ui::text().act_resuming); activity();
    }
    void resume_session(std::string id) {
        if (busy_) return;
        if (auto accepted = runtime_.resume(id); !accepted) {
            toast(accepted.error().message, tui::Notice::Severity::warn);
            return;
        }
        busy(true); phase_ = std::string(ui::text().act_resuming) + id.substr(0, 8); activity();
    }

    void model_panel() {
        if (busy_) return;
        std::vector<Panel::Row> rows;
        int initial = 0;
        int index = 0;
        for (const auto& model : models_) {
            const bool current = model.name == model_name_;
            if (current) initial = index;
            rows.push_back({model.name, model.kind + "   " + model.model,
                            current ? std::string(ui::text().panel_current) : "", true,
                            [this, name = model.name] { switch_model(name); }});
            ++index;
        }
        panel_.open(std::string(ui::text().panel_model), std::move(rows),
                    std::string(ui::text().panel_model_footer), true, {}, {}, initial,
                    [this] { add_model(); });
    }
    void add_model() {
        if (busy_) return;
        model_dialog_.open(window_tokens_, provider_kinds_,
                           [this](agent::ModelInput model) {
            if (auto accepted = runtime_.add_model(std::move(model)); !accepted) {
                toast(accepted.error().message, tui::Notice::Severity::warn);
                return;
            }
            busy(true); phase_ = std::string(ui::text().act_adding_model); activity();
        });
    }
    void switch_model(const std::string& name) {
        if (busy_ || name == model_name_) return;
        if (auto accepted = runtime_.select_model(name); !accepted) {
            toast(accepted.error().message, tui::Notice::Severity::warn);
            return;
        }
        busy(true); phase_ = std::string(ui::text().act_switching_model); activity();
    }

    void command_panel() {
        std::vector<Panel::Row> rows;
        for (const auto& command : keys_.commands()) {
            const auto meta = std::ranges::find_if(command_ui_, [&](const CommandUi& item) { return item.id == command.id; });
            std::string right;
            if (meta != command_ui_.end()) right = !meta->shortcut.empty() ? meta->shortcut : meta->alias;
            const bool enabled = !command.enabled || command.enabled();
            rows.push_back({command.title, command.category, right, enabled,
                            [this, id = command.id] { run_command(id); }});
        }
        panel_.open(std::string(ui::text().panel_commands), std::move(rows), std::string(ui::text().panel_command_footer));
    }
    void session_panel() {
        panel_.open(std::string(ui::text().panel_sessions), {{std::string(ui::text().panel_loading), {}, {}, false, {}}}, std::string(ui::text().panel_session_footer), false);
        runtime_.query_sessions(50, [this](runtime::Runtime::SessionsResult result) {
            if (!result) {
                rt_.post([this, message = result.error().message] {
                    panel_.close();
                    toast(std::string(ui::text().toast_sessions_failed) + message, tui::Notice::Severity::error);
                });
                return;
            }
            rt_.post([this, sessions = std::move(*result)]() mutable {
                std::vector<Panel::Row> rows;
                for (const auto& session : sessions) {
                    rows.push_back({session.title.empty() ? std::string(ui::text().panel_empty_session) : session.title,
                                    relative_age(session.updated), session.id.substr(0, 4), true,
                                    [this, id = session.id] { resume_session(id); }});
                }
                if (rows.empty()) rows.push_back({std::string(ui::text().panel_no_sessions), {}, {}, false, {}});
                panel_.open(std::string(ui::text().panel_sessions), std::move(rows), std::string(ui::text().panel_session_footer));
            });
        });
    }
    void help_panel() {
        std::vector<Panel::Row> rows;
        for (const auto& command : keys_.commands()) {
            const auto meta = std::ranges::find_if(command_ui_, [&](const CommandUi& item) { return item.id == command.id; });
            std::string key;
            if (meta != command_ui_.end()) key = !meta->shortcut.empty() ? meta->shortcut : meta->alias;
            rows.push_back({key, command.title, command.category, false, {}});
        }
        panel_.open(std::string(ui::text().panel_help), std::move(rows), std::string(ui::text().panel_close), false);
    }
    void permissions_panel() {
        const std::vector<agent::Policy::SessionGrant> grants = runtime_.snapshot().grants;
        std::vector<Panel::Row> rows;
        for (const auto& grant : grants) {
            rows.push_back({grant.description, {}, "revoke", true, [this, id = grant.id] {
                bool removed = false;
                if (auto result = runtime_.revoke_grant(id); result) removed = *result;
                toast(removed ? "Session permission revoked" : "Permission was already absent",
                      removed ? tui::Notice::Severity::info : tui::Notice::Severity::warn);
            }});
        }
        if (rows.empty()) rows.push_back({"No active session permissions", {}, {}, false, {}});
        panel_.open("Session permissions", std::move(rows), "enter revoke · esc close", false);
    }
    tui::ThemeTokens theme_for(std::string_view choice) const {
        if (choice == "follow" && themes_) return themes_->pick(terminal_.caps().background);
        const bool light = choice == "light" || (choice == "follow" &&
            &tui::default_theme(terminal_.caps().background) == &tui::light_theme());
        return themes_ ? (light ? themes_->light : themes_->dark) : builtin_theme(light);
    }
    void theme_panel() {
        completion_.close();
        panel_.close();
        theme_preview_ = true;
        const std::vector<std::pair<std::string_view, std::string_view>> choices = {
            {ui::text().panel_dark, "dark"}, {ui::text().panel_light, "light"},
            {ui::text().panel_follow, "follow"}};
        int initial = 0;
        std::vector<Panel::Row> rows;
        for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
            const auto [name, id] = choices[i];
            const bool current = id == theme_choice_;
            if (current) initial = i;
            rows.push_back({std::string(name), {}, current ? std::string(ui::text().panel_current) : "",
                true, [this, id] {
                    theme_choice_ = id;
                    apply_theme(theme_for(id));
                }});
        }
        panel_.open(std::string(ui::text().panel_theme), std::move(rows), std::string(ui::text().panel_theme_footer), false,
            [this, choices](int i) { apply_theme(theme_for(choices[i].second)); },
            [this](bool committed) {
                theme_preview_ = false;
                if (!committed) apply_theme(theme_for(theme_choice_));
            }, initial);
    }

    void prompt_changed() { update_completion();  }
    void update_completion() {
        const std::string value = input_->text();
        if (value.starts_with('/') && value.find('\n') == std::string::npos) {
            if (completion_kind_ != "command") {
                completion_kind_ = "command";
                completion_.open(std::string(ui::text().panel_commands), [this](std::string_view query, auto done) {
                    std::vector<Completion::Item> items;
                    for (const auto& meta : command_ui_) {
                        if (meta.alias.empty() || !meta.alias.starts_with(query)) continue;
                        const auto command = std::ranges::find_if(keys_.commands(), [&](const tui::Command& item) {
                            return item.id == meta.id;
                        });
                        items.push_back({meta.alias, meta.alias,
                                         command == keys_.commands().end() ? "" : command->title, {}});
                    }
                    done(std::move(items));
                }, std::string(ui::text().panel_complete_footer),
                [this](const Completion::Item& item, bool tab) {
                    input_->set_text(tab ? item.value : "");
                    if (!tab) submit(item.value);
                });
            }
            completion_.refresh(value); return;
        }
        const std::size_t token = value.find_last_of(" \t\r\n");
        const std::size_t begin = token == std::string::npos ? 0 : token + 1;
        if (begin < value.size() && value[begin] == '@') {
            if (completion_kind_ != "file") {
                completion_kind_ = "file";
                completion_.open(std::string(ui::text().panel_files), [this](std::string_view query, auto done) {
                    rt_.cancel(file_debounce_);
                    file_debounce_ = rt_.after(80ms, [this, query = std::string(query), done = std::move(done)]() mutable {
                        runtime_.query_files(query, 8,
                                             [done = std::move(done), query](runtime::Runtime::FilesResult result) mutable {
                            std::vector<Completion::Item> items;
                            if (result) {
                                for (const auto& candidate : *result) {
                                    items.push_back({candidate.path, candidate.path, {},
                                                     subsequence_hits(query, candidate.path)});
                                }
                            }
                            done(std::move(items));
                        });
                    });
                }, std::string(ui::text().panel_file_footer),
                [this](const Completion::Item& item, bool) {
                    std::string text = input_->text();
                    const std::size_t split = text.find_last_of(" \t\r\n");
                    text.erase(split == std::string::npos ? 0 : split + 1);
                    text += item.value + " "; input_->set_text(std::move(text));
                });
            }
            completion_.refresh(std::string_view(value).substr(begin + 1)); return;
        }
        completion_.close();
    }

    void interrupt() { if (busy_) runtime_.cancel(); }
    void cancel() {
        if (completion_.visible()) { completion_.close(); return; }
        if (panel_.visible()) { panel_.close(); return; }
        if (busy_) { interrupt(); return; }
        if (active_ != 0) { show_pane(0); return; }
        if (!input_->text().empty()) { input_->set_text({}); prompt_changed(); return; }
        const auto now = Clock::now();
        if (last_cancel_ && now - *last_cancel_ < 1s) { exit(); return; }
        last_cancel_ = now; toast(std::string(ui::text().toast_exit));
    }
    void cycle_permission() {
        if (auto result = runtime_.cycle_permission(); !result)
            toast(result.error().message, tui::Notice::Severity::warn);
    }
    void toggle_plan() {
        if (busy_) return;
        if (auto result = runtime_.toggle_planning(); !result)
            toast(result.error().message, tui::Notice::Severity::warn);
    }
    void exit() {
        exiting_ = true; rt_.quit();
    }

    runtime::Runtime& runtime_;
    std::optional<ThemeSet> themes_;
    bool resumed_ = false;
    std::vector<agent::Event> replay_;
    std::vector<agent::Event> history_;
    std::string initial_prompt_;
    std::vector<agent::PublicModel> models_;
    std::vector<agent::ProviderKindInfo> provider_kinds_;
    tui::ThemeTokens theme_ = tui::dark_theme();
    uint32_t theme_epoch_ = 0;
    std::string theme_choice_ = "follow";
    bool theme_preview_ = false;
    ScrollFrame* frame_ = nullptr;
    tui::Activity* activity_ = nullptr;
    QueueLabel* queue_label_ = nullptr;
    PromptBox* input_ = nullptr;
    StatusLine* status_ = nullptr;
    SidePanel* side_ = nullptr;
    bool titled_ = false;
    tui::Terminal terminal_;
    tui::LayerStack root_;
    tui::Runtime rt_;
    std::vector<Pane> panes_;
    std::size_t active_ = 0;
    std::map<std::string, std::size_t> task_panes_;
    PromptInput prompt_;
    tui::Keymap keys_;
    ApprovalDialog dialog_;
    ModelDialog model_dialog_;
    ToastStack toasts_;
    Panel panel_;
    Completion completion_;
    std::string id_, error_, phase_, project_path_, branch_, completion_kind_;
    std::filesystem::path project_root_;
    std::string active_interaction_, model_name_, model_label_;
    agent::PermissionMode mode_ = agent::PermissionMode::ask;
    bool planning_ = false, read_only_ = false;
    std::size_t worked_tokens_ = 0, token_limit_ = 0, window_tokens_ = 0;
    int trigger_percent_ = 80;
    std::vector<agent::McpServerState> mcp_states_;
    std::vector<CommandUi> command_ui_;
    struct Running { std::string id, summary; Clock::time_point begin; };
    std::vector<Running> running_;
    Clock::time_point step_begin_{};
    std::optional<Clock::time_point> last_cancel_;
    tui::TimerId activity_timer_ = 0, mcp_timer_ = 0, file_debounce_ = 0;
    bool busy_ = false, exiting_ = false, signal_exit_ = false;
};
} // namespace

int run_interactive(runtime::Runtime& runtime, const InteractiveOptions& options,
                    agent::Interrupts& interrupts) {
    std::optional<ThemeSet> themes;
    const std::filesystem::path theme_file = runtime.theme_file();
    if (!theme_file.empty()) themes = load_theme(theme_file);

    runtime::RuntimeSnapshot snapshot = runtime.snapshot();
    std::mutex history_mutex;
    std::condition_variable history_cv;
    bool history_done = false;
    runtime::Runtime::HistoryResult history_result;
    if (!snapshot.session_id.empty()) {
        runtime.query_history(snapshot.session_id, [&](runtime::Runtime::HistoryResult result) {
            {
                const std::lock_guard lock(history_mutex);
                history_result = std::move(result);
                history_done = true;
            }
            history_cv.notify_all();
        });
        std::unique_lock lock(history_mutex);
        history_cv.wait(lock, [&] { return history_done; });
    }

    std::vector<agent::Event> history;
    if (history_result) history = std::move(*history_result);
    Shell shell(runtime, snapshot, runtime::StartResult{options.resumed, options.replay},
                std::move(history), std::move(themes), options);
    runtime.set_frontend(&shell);
    return shell.run(interrupts);
}
} // namespace dagent::ui
