#include "ui/shell.hpp"
#include "ui/strings.hpp"
#include "ui/display.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <format>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

#include "agent/agent.hpp"
#include "agent/record.hpp"
#include "base/log.hpp"
#include "session/session.hpp"
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
#include "workspace/context.hpp"
#include "workspace/search.hpp"

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
    explicit ScrollFrame(std::unique_ptr<tui::Scrollback> scroll)
        : scroll_(std::move(scroll)) { adopt(*this, *scroll_); }
    ~ScrollFrame() override { disown(*scroll_); }
    tui::Size measure(tui::Size available) const override { return available; }
    void layout(tui::Rect area) override {
        tui::Widget::layout(area); scroll_->layout({0, 0, area.w, area.h});
    }
    void render(tui::Surface& surface) override {
        if (scroll_->dirty_tree()) {
            scroll_->render(surface); scroll_->clear_dirty();
        }
        if (!scroll_->pinned() && scroll_->unseen_rows() > 0 && surface.rows() > 0) {
            const std::string text = format_text(ui::text().card_unseen, scroll_->unseen_rows());
            const int x = std::max(0, surface.cols() - display_width(text));
            surface.text(x, surface.rows() - 1, text, scroll_->theme().text_muted);
        }
        clear_dirty();
    }
    bool dirty_tree() const noexcept override { return dirty_ || scroll_->dirty_tree(); }
    bool needs_layout() const noexcept override { return layout_dirty_ || scroll_->needs_layout(); }
    void invalidate_tree() noexcept override { invalidate(); scroll_->invalidate_tree(); }
    void invalidate_rect(tui::Rect rect) override { scroll_->invalidate_rect(rect); Widget::invalidate_rect(rect); }
    tui::Widget* hit_test(tui::Point point) const noexcept override {
        if (auto* hit = scroll_->hit_test(point)) return hit;
        return Widget::hit_test(point);
    }
private:
    std::unique_ptr<tui::Scrollback> scroll_;
};

class JobQueue {
public:
    void push(std::function<void()> job) {
        std::lock_guard lock(mutex_);
        if (closed_) return;
        jobs_.push_back(std::move(job)); cv_.notify_one();
    }
    std::optional<std::function<void()>> pop(std::stop_token stop) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, stop, [&] { return closed_ || !jobs_.empty(); });
        if (closed_ || stop.stop_requested()) return std::nullopt;
        auto job = std::move(jobs_.front()); jobs_.pop_front(); return job;
    }
    void close() {
        std::lock_guard lock(mutex_); closed_ = true; jobs_.clear(); cv_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<std::function<void()>> jobs_;
    bool closed_ = false;
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

class Shell final : public tui::EventHandler {
public:
    Shell(agent::Setup setup, std::unique_ptr<agent::Agent> agent,
          std::vector<agent::Event> history, std::optional<ThemeSet> themes,
          std::optional<std::filesystem::path> theme_file, bool resumed,
          const InteractiveOptions& options)
        : setup_(std::move(setup)), agent_(std::move(agent)), models_(options.models),
          resolve_model_(options.resolve_model), add_model_(options.add_model),
          mode_(setup_.permission_mode), planning_(setup_.planning),
          themes_(std::move(themes)),
          theme_file_(std::move(theme_file)), root_(layout()), rt_(terminal_, root_),
          transcript_(scroll_->document(), [this](const tools::TodoView& value) { update_todo(value); }),
          prompt_(*input_, [this](std::string text) { submit(std::move(text)); },
                  [this] { recall(); }, [this] { prompt_changed(); },
                  [this] { return completion_.visible(); }),
          keys_(rt_), mouse_(rt_, *scroll_), dialog_(rt_, [this] { interrupt(); }), model_dialog_(rt_),
          toasts_(rt_), panel_(rt_),
          completion_(rt_, *input_, [this] { completion_kind_.clear();  }) {
        id_ = agent_->meta().id;
        project_path_ = display_path(setup_.project_root);
        status_->project(project_path_);
        status_->set_trigger(setup_.options.context.compaction_trigger_percent);
        side_->set_project(project_path_, {});
        side_->set_version(DAGENT_VERSION);
        transcript_.set_session(mode_label(), setup_.provider.model);
        update_prompt_footer();
        rt_.set_focus(&prompt_, input_); rt_.set_global(*this);
        terminal_.set_mouse(true); // 不开这一行，滚轮与拖选的转义序列根本不会上报
        rt_.bind_mouse(*scroll_, mouse_);
        register_commands();
        const auto initial_light = &tui::default_theme(terminal_.caps().background) == &tui::light_theme();
        theme_choice_ = themes_ && theme_file_ ? theme_file_->string() + (initial_light ? ":light" : ":dark") : "follow";
        apply_theme(themes_ ? themes_->pick(terminal_.caps().background) : builtin_theme(initial_light));
        rt_.on_caps([this](const tui::Terminal::Caps& caps) {
            if (theme_chosen_) return;
            const bool light = &tui::default_theme(caps.background) == &tui::light_theme();
            theme_choice_ = themes_ && theme_file_ ? theme_file_->string() + (light ? ":light" : ":dark") : "follow";
            apply_theme(themes_ ? themes_->pick(caps.background) : builtin_theme(light));
        });
        reset_transcript(resumed, history.size());
        for (const auto& event : history) apply(event);
        worker_ = std::jthread([this](std::stop_token stop) {
            try {
                while (auto job = jobs_.pop(stop)) (*job)();
            } catch (const std::exception& e) { worker_failed(e.what()); }
            catch (...) { worker_failed("agent thread crashed"); }
        });
        refresh_project();
        watch_mcp();

    }

    ~Shell() override {
        turn_stop_.request_stop(); jobs_.close(); worker_.request_stop();
        rt_.cancel(file_debounce_); terminal_.restore();
        if (worker_.joinable()) worker_.join();
        rt_.unbind_mouse(*scroll_);
    }

    int run(const std::string& initial, agent::Interrupts& interrupts) {
        interrupts.graceful = true;
        std::stop_callback signal(interrupts.stop.get_token(), [this] {
            rt_.post([this] { signal_exit_ = true; exit(); });
        });
        if (!initial.empty()) submit(initial);
        rt_.run();
        turn_stop_.request_stop(); jobs_.close(); worker_.request_stop();
        terminal_.restore();
        if (worker_.joinable()) worker_.join();
        agent_.reset();
        interrupts.graceful = false;
        if (!error_.empty()) std::cerr << "failed: " << error_ << '\n';
        std::cout << std::format("Session {0} saved. Resume with: dagent -r {0}\n", id_);
        return !error_.empty() ? 1 : signal_exit_ ? 130 : 0;
    }

    bool on_event(const tui::Event& event) override {
        if (event.kind == tui::Event::Kind::resize) {
            transcript_.set_todo_narrow(event.size.cols < 80);
            update_todo_status(event.size.cols);
        }
        return keys_.on_event(event);
    }

private:
    struct CommandUi { std::string id, shortcut, alias; };

    std::unique_ptr<tui::Widget> layout() {
        auto main = std::make_unique<tui::Container>();
        auto add_centered = [&]<class T>(T*& target, tui::Constraint constraint) {
            auto widget = std::make_unique<T>(); target = widget.get();
            main->add(constraint, centered(std::move(widget), 88, theme_));
        };
        auto scroll = std::make_unique<tui::Scrollback>(); scroll_ = scroll.get();
        main->add({tui::Sizing::flex, 1}, centered(std::make_unique<ScrollFrame>(std::move(scroll)), 88, theme_));
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
        add_command("mode.plan", "Toggle planning mode", std::string(ui::text().cmd_permission_group), "ctrl+g", "/plan", [this] { toggle_plan(); });
        add_command("tools.expand", std::string(ui::text().cmd_tools), std::string(ui::text().cmd_transcript), "ctrl+o", {}, [this] { transcript_.toggle_tools(); });
        add_command("thoughts.toggle", std::string(ui::text().cmd_thoughts), std::string(ui::text().cmd_transcript), "ctrl+r", {}, [this] { transcript_.toggle_thoughts(); });
        add_command("todo.toggle", std::string(ui::text().cmd_todo), std::string(ui::text().cmd_view), "ctrl+t", {}, [this] { toggle_todo(); });
        add_command("scroll.up", std::string(ui::text().cmd_page_up), std::string(ui::text().cmd_transcript), "pageup", {}, [this] { scroll_->scroll_pages(-1); });
        add_command("scroll.down", std::string(ui::text().cmd_page_down), std::string(ui::text().cmd_transcript), "pagedown", {}, [this] { scroll_->scroll_pages(1); });
        add_command("scroll.home", std::string(ui::text().cmd_home), std::string(ui::text().cmd_transcript), "home", {}, [this] { scroll_->scroll_home(); });
        add_command("scroll.end", std::string(ui::text().cmd_end), std::string(ui::text().cmd_transcript), "end", {}, [this] { scroll_->scroll_end(); });
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
        transcript_.clear();
        if (resumed) transcript_.resumed(id_, messages, std::string(ui::text().panel_just_now));
        else transcript_.banner(DAGENT_VERSION, project_path_, branch_, setup_.provider.model,
                                static_cast<int>(setup_.mcp_servers.size()));
        transcript_.set_todo_narrow(terminal_.size().cols < 80);
    }
    void refresh_project() {
        jobs_.push([this] {
            const workspace::Environment env = workspace::collect_environment(setup_.cwd);
            std::string branch;
            if (env.git) {
                branch = env.git->branch;
                if (!env.git->status_summary.empty()) branch += "*";
            }
            rt_.post([this, branch = std::move(branch)] {
                branch_ = branch; side_->set_project(project_path_, branch_);
            });
        });
    }
    void worker_failed(std::string error) {
        base::logger("ui")->error("agent thread exited: {}", error);
        rt_.post([this, error = std::move(error)] {
            error_ = error; transcript_.apply(agent::Notice{agent::Notice::Level::error, error}); exit();
        });
    }
    void toast(std::string text, tui::Notice::Severity severity = tui::Notice::Severity::info) {
        toasts_.show(severity, std::move(text));
    }
    void apply_theme(const tui::ThemeTokens& selected) {
        theme_ = resolve_theme(selected); theme_.epoch = ++theme_epoch_;
        scroll_->set_theme(theme_); activity_->set_theme(theme_);
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
        std::vector<agent::ServerState> states;
        { std::lock_guard lock(agent_mutex_); states = agent_->mcp_states(); }
        side_->set_mcp(mcp_label(states));
        for (const auto& state : states) {
            using Status = agent::ServerState::Status;
            if (state.status == Status::connecting || state.status == Status::reconnecting) return true;
        }
        return false;
    }
    void watch_mcp() {
        if (setup_.mcp_servers.empty() || mcp_timer_) return;
        const bool pending = update_mcp();
        if (!pending && !busy_) return;
        mcp_timer_ = rt_.every(200ms, [this] {
            const bool pending_now = update_mcp();
            if (pending_now || busy_) return true;
            mcp_timer_ = 0; return false;
        });
    }
    void busy(bool value) {
        busy_ = value; rt_.cancel(activity_timer_); activity_timer_ = 0;
        input_->set_active(value); update_prompt_footer();
        if (value) {
            step_begin_ = Clock::now(); phase_ = std::string(ui::text().act_thinking);
            activity_timer_ = rt_.every(100ms, [this] { activity(); activity_->tick(); return true; });
        } else running_.clear();
        watch_mcp(); activity();
    }
    void apply(const agent::Event& event) {
        transcript_.apply(event);
        std::visit(Overloaded{
            [&](const agent::StepStarted&) { phase_ = std::string(ui::text().act_thinking); step_begin_ = Clock::now(); },
            [&](const agent::TextDelta&) { phase_ = std::string(ui::text().act_generating); },
            [&](const agent::ReasoningDelta&) { phase_ = std::string(ui::text().act_generating); },
            [&](const agent::ToolPending& e) { phase_ = std::string(ui::text().act_preparing) + e.name; },
            [&](const agent::ToolStarted& e) { running_.push_back({e.id, e.summary, Clock::now()}); },
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
                transcript_.set_session(mode_label(), setup_.provider.model);
                update_prompt_footer();
            },
            [&](const agent::TurnEnded&) { dialog_.close(); busy(false); drain(); },
            [](const auto&) {}
        }, event);
        activity();
    }

    void update_todo(const tools::TodoView& value) {
        side_->set_items(value.items);
        update_todo_status(terminal_.size().cols);
    }
    void update_todo_status(int columns) {
        const auto [done, total] = side_->progress();
        status_->todo(done, total, total > 0 && (side_->collapsed() || columns < 80));
    }
    void toggle_todo() {
        side_->set_collapsed(!side_->collapsed());
        transcript_.set_todo_collapsed(side_->collapsed());
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
        input_->set_footer(format_text(ui::text().box_footer, mode_label(), setup_.provider.model));
        input_->set_footer_tone(!planning_ && mode_ == agent::PermissionMode::unrestricted, planning_);
    }
    static std::string mcp_label(const std::vector<agent::ServerState>& states) {
        if (states.empty()) return {};
        const auto ready = std::ranges::count_if(states, [](const agent::ServerState& state) {
            return state.status == agent::ServerState::Status::ready;
        });
        return format_text(ui::text().status_mcp, ready, states.size());
    }

    void refresh_queue() {
        std::string label;
        if (!pending_.empty()) {
            label = format_text(ui::text().queue_count, pending_.size());
            const std::size_t first = pending_.size() > 2 ? pending_.size() - 2 : 0;
            for (std::size_t i = first; i < pending_.size(); ++i)
                label += std::format("\n{} {}", i - first + 1, pending_[i].substr(0, pending_[i].find('\n')));
        }
        queue_label_->set_text(std::move(label));
    }
    void recall() {
        if (pending_.empty()) return;
        input_->set_text(std::move(pending_.back())); pending_.pop_back();
        refresh_queue(); prompt_changed();
    }
    void submit(std::string text) {
        if (exiting_) return;
        if (!text.starts_with('/')) set_session_title(text);
        if (busy_ && text == "/model") { toast(std::string(ui::text().toast_model_busy), tui::Notice::Severity::warn); return; }
        pending_.push_back(std::move(text)); refresh_queue();  drain();
    }
    /// 侧栏标题取本会话第一条真实输入的首行。
    void set_session_title(const std::string& input) {
        if (titled_) return;
        const std::string line = input.substr(0, input.find('\n'));
        if (line.empty()) return;
        side_->set_title(fit_columns(line, 40)); titled_ = true;
    }
    void drain() {
        while (!busy_ && !exiting_ && !pending_.empty()) {
            auto text = std::move(pending_.front()); pending_.pop_front(); refresh_queue();
            if (text.starts_with('/')) { command(text); continue; }
            busy(true); turn_stop_ = std::stop_source{};
            const auto token = turn_stop_.get_token();
            jobs_.push([this, text = std::move(text), token] {
                const agent::Sink sink = [this](const agent::Event& event) {
                    rt_.post([this, event] { if (!exiting_) apply(event); });
                };
                agent_->run_turn(text, sink, [this](const agent::Approval& approval, std::stop_token stop) {
                    return approve(rt_, dialog_, approval, stop);
                }, [this](const agent::Question& question, std::stop_token stop) {
                    return ask(rt_, dialog_, question, stop);
                }, token);
            });
        }
    }
    void command(const std::string& text) {
        const auto ui = std::ranges::find_if(command_ui_, [&](const CommandUi& item) { return item.alias == text; });
        if (ui == command_ui_.end()) toast(std::string(ui::text().toast_unknown_command) + text, tui::Notice::Severity::warn);
        else run_command(ui->id);
    }
    void compact() {
        if (busy_) return;
        busy(true); phase_ = std::string(ui::text().act_compacting); activity(); turn_stop_ = std::stop_source{};
        const auto token = turn_stop_.get_token();
        jobs_.push([this, token] {
            const auto status = agent_->compact([this](const agent::Event& event) {
                rt_.post([this, event] { if (!exiting_) apply(event); });
            }, token);
            rt_.post([this, status] {
                if (exiting_) return;
                if (status == agent::TurnStatus::interrupted) toast(std::string(ui::text().toast_compact_cancelled));
                busy(false); drain();
            });
        });
    }
    void new_session() {
        if (busy_) return;
        busy(true);
        jobs_.push([this] {
            auto setup = setup_;
            { std::lock_guard lock(agent_mutex_); setup.permission_mode = mode_; setup.planning = false; }
            auto next = agent::Agent::create(std::move(setup));
            std::string id = next->meta().id;
            { std::lock_guard lock(agent_mutex_); next->set_permission_mode(mode_); next->set_plan_mode(false); agent_.swap(next); }
            rt_.post([this, id = std::move(id)] {
                id_ = id; planning_ = false; reset_transcript(); side_->set_title({});
                transcript_.set_session(mode_label(), setup_.provider.model); update_prompt_footer();
                refresh_project(); busy(false); drain();
            });
        });
    }
    void resume_session(std::string id) {
        if (busy_) return;
        busy(true); phase_ = std::string(ui::text().act_resuming) + id.substr(0, 8); activity();
        jobs_.push([this, id = std::move(id)] {
            try {
                std::vector<agent::Event> history;
                auto setup = setup_; setup.permission_mode = mode_; setup.planning = planning_;
                auto next = agent::Agent::resume(setup, id, [&](const agent::Event& event) { history.push_back(event); });
                { std::lock_guard lock(agent_mutex_); next->set_permission_mode(mode_); agent_.swap(next); }
                rt_.post([this, id, history = std::move(history)]() mutable {
                    id_ = id; reset_transcript(true, history.size());
                    for (const auto& event : history) apply(event);
                    refresh_project(); busy(false); drain();
                });
            } catch (const std::exception& error) {
                rt_.post([this, message = std::string(error.what())] {
                    toast(std::string(ui::text().toast_resume_failed) + message, tui::Notice::Severity::error); busy(false); drain();
                });
            }
        });
    }

    void model_panel() {
        if (busy_) return;
        std::vector<Panel::Row> rows;
        int initial = 0;
        for (const auto& [name, provider] : models_) {
            const bool current = name == setup_.provider.name;
            if (current) initial = static_cast<int>(rows.size());
            rows.push_back({name, provider.kind + "   " + provider.model, current ? std::string(ui::text().panel_current) : "", true,
                            [this, name] { switch_model(name); }});
        }
        panel_.open(std::string(ui::text().panel_model), std::move(rows),
                    std::string(ui::text().panel_model_footer), true, {}, {}, initial,
                    [this] { add_model(); });
    }
    void add_model() {
        if (busy_ || !add_model_) return;
        model_dialog_.open(setup_.options.context.window_tokens,
                           [this](agent::ProviderConfig model) {
            busy(true); phase_ = std::string(ui::text().act_adding_model); activity();
            jobs_.push([this, model = std::move(model)]() mutable {
                try {
                    agent::ProviderConfig saved = add_model_(std::move(model));
                    rt_.post([this, saved = std::move(saved)]() mutable {
                        const std::string name = saved.name;
                        models_[name] = std::move(saved);
                        toast(std::string(ui::text().toast_model_added) + name);
                        busy(false);
                        switch_model(name);
                    });
                } catch (const std::exception& error) {
                    rt_.post([this, message = std::string(error.what())] {
                        toast(std::string(ui::text().toast_model_failed) + message,
                              tui::Notice::Severity::error);
                        busy(false);
                    });
                }
            });
        });
    }
    void switch_model(const std::string& name) {
        if (busy_ || name == setup_.provider.name) return;
        busy(true); phase_ = std::string(ui::text().act_switching_model); activity();
        auto setup = setup_;
        jobs_.push([this, name, setup = std::move(setup)]() mutable {
            try {
                setup.provider = resolve_model_ ? resolve_model_(name) : models_.at(name);
                { std::lock_guard lock(agent_mutex_); setup.permission_mode = mode_; setup.planning = planning_; }
                std::vector<agent::Event> updates;
                auto next = agent::Agent::resume(setup, id_, [&](const agent::Event& event) {
                    if (std::holds_alternative<agent::ContextUpdate>(event) ||
                        std::holds_alternative<agent::Notice>(event)) updates.push_back(event);
                });
                { std::lock_guard lock(agent_mutex_); next->set_permission_mode(mode_); agent_.swap(next); }
                next.reset(); // 可能等待 MCP 线程，在锁外、工作线程销毁。
                rt_.post([this, provider = std::move(setup.provider), updates = std::move(updates)]() mutable {
                    setup_.provider = std::move(provider);
                    models_[setup_.provider.name] = setup_.provider;
                    transcript_.set_session(mode_label(), setup_.provider.model);
                    update_prompt_footer();
                    for (const auto& event : updates) apply(event);
                    toast(std::string(ui::text().toast_model_selected) + setup_.provider.name);
                    busy(false); drain();
                });
            } catch (const std::exception& error) {
                rt_.post([this, message = std::string(error.what())] {
                    toast(std::string(ui::text().toast_model_failed) + message, tui::Notice::Severity::error);
                    busy(false); drain();
                });
            }
        });
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
        jobs_.push([this] {
            try {
                auto sessions = session::list(setup_.session, setup_.cwd, 50);
                rt_.post([this, sessions = std::move(sessions)]() mutable {
                    std::vector<Panel::Row> rows;
                    for (const auto& session : sessions) {
                        rows.push_back({session.title.empty() ? std::string(ui::text().panel_empty_session) : session.title,
                                        relative_age(session.updated), session.meta.id.substr(0, 4), true,
                                        [this, id = session.meta.id] { resume_session(id); }});
                    }
                    if (rows.empty()) rows.push_back({std::string(ui::text().panel_no_sessions), {}, {}, false, {}});
                    panel_.open(std::string(ui::text().panel_sessions), std::move(rows), std::string(ui::text().panel_session_footer));
                });
            } catch (const std::exception& error) {
                rt_.post([this, message = std::string(error.what())] {
                    panel_.close(); toast(std::string(ui::text().toast_sessions_failed) + message, tui::Notice::Severity::error);
                });
            }
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
    void theme_panel() {
        completion_.close();
        panel_.close();
        theme_chosen_ = true;
        const auto request = ++theme_request_;
        panel_.open(std::string(ui::text().panel_theme), {{std::string(ui::text().panel_loading), {}, {}, false, {}}}, std::string(ui::text().panel_close), false, {},
                    [this](bool) { ++theme_request_; });
        const std::filesystem::path directory = theme_file_ && !theme_file_->empty()
            ? theme_file_->parent_path() : setup_.project_root / "config/themes";
        jobs_.push([this, directory, request] {
            auto files = list_themes(directory);
            rt_.post([this, request, files = std::move(files)] {
                if (exiting_ || request != theme_request_ || !panel_.visible()) return;
                struct Choice { std::string name, id; std::optional<tui::ThemeTokens> tokens; };
                auto choices = std::make_shared<std::vector<Choice>>();
                const bool light = &tui::default_theme(terminal_.caps().background) == &tui::light_theme();
                choices->push_back({std::string(ui::text().panel_dark), "dark", builtin_theme(false)});
                choices->push_back({std::string(ui::text().panel_light), "light", builtin_theme(true)});
                choices->push_back({std::string(ui::text().panel_follow), "follow", builtin_theme(light)});
                for (const auto& file : files) {
                    if (!file.loaded) {
                        choices->push_back({file.name, file.path.string(), std::nullopt});
                        continue;
                    }
                    choices->push_back({file.name + " · dark", file.path.string() + ":dark", file.loaded->dark});
                    choices->push_back({file.name + " · light", file.path.string() + ":light", file.loaded->light});
                }
                const tui::ThemeTokens before = theme_;
                int initial = 0;
                std::vector<Panel::Row> rows;
                for (int i = 0; i < static_cast<int>(choices->size()); ++i) {
                    const auto& choice = (*choices)[i];
                    const bool current = choice.id == theme_choice_;
                    if (current) initial = i;
                    rows.push_back({choice.name, {}, !choice.tokens ? std::string(ui::text().panel_failed) : current ? std::string(ui::text().panel_current) : "",
                        choice.tokens.has_value(), [this, choices, i] {
                            const auto& selected = (*choices)[i];
                            theme_choice_ = selected.id;
                            apply_theme(*selected.tokens);
                        }});
                }
                panel_.open(std::string(ui::text().panel_theme), std::move(rows), std::string(ui::text().panel_theme_footer), false,
                    [this, choices](int i) {
                        if (const auto& tokens = (*choices)[i].tokens) apply_theme(*tokens);
                    }, [this, before](bool committed) { if (!committed) apply_theme(before); }, initial);
            });
        });
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
                        jobs_.push([this, query = std::move(query), done = std::move(done)]() mutable {
                            if (file_cache_.empty()) {
                                workspace::FilesQuery files;
                                files.root = setup_.project_root; files.max_files = 5000;
                                file_cache_ = workspace::files(files, setup_.search);
                            }
                            const auto ranked = workspace::fuzzy_rank(query, file_cache_, 8);
                            std::vector<Completion::Item> items;
                            for (std::size_t index : ranked) {
                                const std::string& path = file_cache_[index];
                                items.push_back({path, path, {}, subsequence_hits(query, path)});
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

    void interrupt() { if (busy_) turn_stop_.request_stop(); }
    void cancel() {
        if (completion_.visible()) { completion_.close(); return; }
        if (panel_.visible()) { panel_.close(); return; }
        if (busy_) { interrupt(); return; }
        if (!input_->text().empty()) { input_->set_text({}); prompt_changed(); return; }
        const auto now = Clock::now();
        if (last_cancel_ && now - *last_cancel_ < 1s) { exit(); return; }
        last_cancel_ = now; toast(std::string(ui::text().toast_exit));
    }
    void cycle_permission() {
        std::lock_guard lock(agent_mutex_);
        switch (mode_) {
        case agent::PermissionMode::ask:
            mode_ = agent::PermissionMode::workspace;
            break;
        case agent::PermissionMode::workspace:
            mode_ = agent::PermissionMode::unrestricted;
            break;
        case agent::PermissionMode::unrestricted:
            mode_ = agent::PermissionMode::ask;
            break;
        }
        agent_->set_permission_mode(mode_);
        transcript_.set_session(mode_label(), setup_.provider.model); update_prompt_footer();
    }
    void toggle_plan() {
        if (busy_) return;
        std::lock_guard lock(agent_mutex_);
        planning_ = !planning_;
        agent_->set_plan_mode(planning_);
        transcript_.set_session(mode_label(), setup_.provider.model);
        update_prompt_footer();
    }
    void exit() {
        exiting_ = true; pending_.clear(); turn_stop_.request_stop(); jobs_.close();
        worker_.request_stop(); rt_.quit();
    }

    agent::Setup setup_;
    std::unique_ptr<agent::Agent> agent_;
    std::map<std::string, agent::ProviderConfig> models_;
    std::function<agent::ProviderConfig(const std::string&)> resolve_model_;
    std::function<agent::ProviderConfig(agent::ProviderConfig)> add_model_;
    std::mutex agent_mutex_;
    agent::PermissionMode mode_ = agent::PermissionMode::ask;
    bool planning_ = false;
    std::optional<ThemeSet> themes_;
    std::optional<std::filesystem::path> theme_file_;
    tui::ThemeTokens theme_ = tui::dark_theme();
    uint32_t theme_epoch_ = 0;
    std::string theme_choice_ = "follow";
    bool theme_chosen_ = false;
    std::uint64_t theme_request_ = 0;
    tui::Scrollback* scroll_ = nullptr;
    tui::Activity* activity_ = nullptr;
    QueueLabel* queue_label_ = nullptr;
    PromptBox* input_ = nullptr;
    StatusLine* status_ = nullptr;
    SidePanel* side_ = nullptr;
    bool titled_ = false;
    tui::Terminal terminal_;
    tui::LayerStack root_;
    tui::Runtime rt_;
    Transcript transcript_;
    PromptInput prompt_;
    tui::Keymap keys_;
    tui::ScrollbackMouse mouse_;
    ApprovalDialog dialog_;
    ModelDialog model_dialog_;
    ToastStack toasts_;
    Panel panel_;
    Completion completion_;
    JobQueue jobs_;
    std::jthread worker_;
    std::stop_source turn_stop_;
    std::string id_, error_, phase_, project_path_, branch_, completion_kind_;
    std::deque<std::string> pending_;
    std::vector<CommandUi> command_ui_;
    std::vector<std::string> file_cache_;
    struct Running { std::string id, summary; Clock::time_point begin; };
    std::vector<Running> running_;
    Clock::time_point step_begin_{};
    std::optional<Clock::time_point> last_cancel_;
    tui::TimerId activity_timer_ = 0, mcp_timer_ = 0, file_debounce_ = 0;
    bool busy_ = false, exiting_ = false, signal_exit_ = false;
};
} // namespace

int run_interactive(agent::Setup setup, const InteractiveOptions& options, agent::Interrupts& interrupts) {
    std::optional<ThemeSet> themes;
    if (options.theme_file && !options.theme_file->empty()) themes = load_theme(*options.theme_file);
    std::vector<agent::Event> history;
    std::unique_ptr<agent::Agent> agent;
    const bool resumed = options.resume_id.has_value() || options.continue_last;
    if (resumed) {
        const auto prefix = options.resume_id ? std::optional<std::string_view>(*options.resume_id) : std::nullopt;
        const auto id = agent::resolve_session_id(setup.session, setup.cwd, prefix);
        agent = agent::Agent::resume(setup, id, [&](const agent::Event& event) { history.push_back(event); });
    } else agent = agent::Agent::create(setup);
    Shell shell(std::move(setup), std::move(agent), std::move(history), std::move(themes),
                options.theme_file, resumed, options);
    return shell.run(options.initial_prompt, interrupts);
}
} // namespace dagent::ui
