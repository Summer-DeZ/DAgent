#include "ui/shell.hpp"

#include "ui/strings.hpp"
#include "ui/welcome.hpp"
#include "ui/display.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/platform.hpp"
#include "base/path.hpp"
#include "base/log.hpp"
#include "base/overloaded.hpp"
#include "tui/document.hpp"
#include "ui/approval.hpp"
#include "ui/callback_gate.hpp"
#include "ui/center.hpp"
#include "ui/completion_controller.hpp"
#include "ui/model_dialog.hpp"
#include "ui/model_picker.hpp"
#include "ui/memory_browser.hpp"
#include "ui/panel.hpp"
#include "ui/prompt_box.hpp"
#include "ui/prompt_input.hpp"
#include "ui/header_bar.hpp"
#include "ui/history_pager.hpp"
#include "ui/hint_bar.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/theme_config.hpp"
#include "ui/toast.hpp"
#include "ui/side_panel.hpp"
#include "ui/transcript.hpp"
#include "ui/turn_meter.hpp"

#ifndef DAGENT_VERSION
#define DAGENT_VERSION "dev"
#endif

namespace dagent::ui {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::string local_time() {
    const auto now = std::time(nullptr);
    std::tm local{}; base::local_time(now, local);
    char value[6]; std::strftime(value, sizeof(value), "%H:%M", &local);
    return value;
}
class QueueLabel final : public tui::Widget {
public:
    void set_queue(const std::vector<protocol::QueueItem>& queue) { queue_ = queue; invalidate_layout(); }
    void set_theme(const tui::ThemeTokens& t) { theme_ = &t; invalidate(); }
    bool outline = false;
    tui::Size measure(tui::Size available) const override { return {available.cols, queue_.empty() ? 0 : 1}; }
    void render(tui::Surface& s) override {
        const auto& t = *theme_;
        s.fill({0, 0, s.cols(), s.rows()}, U' ', t.background);
        if (queue_.empty()) return;
        const auto hint = format_text(ui::text().queue_recall, icons().up);
        const int right = s.cols() - display_width(hint);
        int x = s.text(0, 0, ui::text().queue_label, t.text_muted) + 1;
        const int count = std::min(3, static_cast<int>(queue_.size()));
        const int gap = outline ? 2 : 1;
        int shown = 0;
        for (; shown < count; ++shown) {
            const int remaining = count - shown;
            const int more = queue_.size() > static_cast<size_t>(count) ? display_width(std::format("+{}", queue_.size() - count)) + 1 : 0;
            const int room = (right - x - more - 1) / remaining - gap - (outline ? 0 : 2);
            if (room < 1) break;
            const auto& value = queue_[shown].text_preview;
            const auto label = fit_columns(std::string_view(value).substr(0, value.find('\n')), std::min(24, room));
            x = draw_chip(s, x, 0, label, t.text, t, outline, t.background.bg) + gap;
        }
        if (queue_.size() > static_cast<size_t>(shown))
            s.text(x, 0, fit_columns(std::format("+{}", queue_.size() - shown), right - x - 1), t.text_muted);
        s.text(right, 0, hint, t.text_muted);
    }
private:
    std::vector<protocol::QueueItem> queue_;
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
};

class AgentTabs final : public tui::Widget, public tui::EventHandler {
public:
    std::vector<AgentInfo> tabs;
    size_t active = 0;
    int frame = 0;
    std::function<void(size_t)> select;
    void set_theme(const tui::ThemeTokens& t) { theme_ = &t; invalidate(); }
    tui::Size measure(tui::Size a) const override { return {a.cols, tabs.size() > 1 ? 2 : 0}; }
    bool on_event(const tui::Event& e) override {
        if (e.kind != tui::Event::Kind::mouse || !e.mouse.press || e.mouse.motion || e.mouse.button != 0) return false;
        for (const auto& hit : hits_) if (e.mouse.x >= hit.begin && e.mouse.x < hit.end) { select(hit.index); return true; }
        return false;
    }
    void render(tui::Surface& s) override {
        const auto& t = *theme_; hits_.clear();
        s.fill({0, 0, s.cols(), s.rows()}, U' ', t.background);
        if (tabs.size() < 2) return;
        const auto hint = ui::text().agent_switch;
        const int right = std::max(0, s.cols() - display_width(hint) - 1);
        const int label_limit = std::max(4, (right - 10) / 2);
        const auto label = [&](size_t i) {
            return fit_columns(i == 0 ? tabs[i].name : std::string(ui::text().agent_task) + std::string(icons().separator) + tabs[i].name, label_limit);
        };
        size_t first = 1;
        int required = 0;
        for (size_t i = 1; i <= active; ++i) required += display_width(label(i)) + 4;
        while (first < active && required > right - 10) { required -= display_width(label(first)) + 4; ++first; }
        int x = 1;
        for (size_t i = 0; i < tabs.size(); i = i == 0 ? first : i + 1) {
            const auto name = label(i); const int width = display_width(name) + 2;
            if (x + width > right - 1) break;
            const auto state = tabs[i].state;
            const auto tone = state == AgentState::running ? t.accent : state == AgentState::failed ? t.error : t.success;
            s.text(x, 0, state == AgentState::running ? icons().spinner[frame] : state == AgentState::failed ? icons().cross : icons().check, tone);
            s.text(x + 2, 0, name, i == active ? t.text : t.text_muted);
            if (i == active) for (int col = x; col < x + width; ++col) s.put(col, 1, icons().horizontal, t.primary);
            hits_.push_back({x, x + width, i}); x += width + 2;
        }
        s.text(right, 0, hint, t.text_muted);
    }
private:
    struct Hit { int begin, end; size_t index; };
    std::vector<Hit> hits_;
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
};

// 重画完整基础层再混色，避免增量帧反复把已经变暗的前景再次混色。
class Page final : public tui::Container {
public:
    std::function<bool()> modal;
    std::function<void(tui::Surface&)> dim;
    bool dirty_tree() const noexcept override {
        return tui::Container::dirty_tree() || (modal && modal() != dimmed_);
    }
    void render(tui::Surface& surface) override {
        invalidate_tree();
        tui::Container::render(surface);
        dimmed_ = modal && modal();
        if (dimmed_) dim(surface);
    }
private:
    bool dimmed_ = false;
};

class Workspace final : public tui::Container {
public:
    Workspace() : tui::Container(Direction::horizontal) {}
    bool left = false;
    std::function<void(int)> resize;
    void layout(tui::Rect area) override {
        if (resize) resize(area.w);
        // Both panel widths are decided from the terminal width before laying out the main area.
        tui::Widget::layout(area);
        const int status = child(1).measure({area.w, area.h}).cols;
        const int navigator = child(2).measure({area.w, area.h}).cols;
        child(0).layout({left ? status : navigator, 0, std::max(0, area.w - status - navigator), area.h});
        child(1).layout({left ? 0 : area.w - status, 0, status, area.h});
        child(2).layout({left ? area.w - navigator : 0, 0, navigator, area.h});
    }
};

class ScrollFrame final : public tui::Widget {
public:
    WelcomeView welcome;
    ScrollFrame() { adopt(*this, welcome); }
    ~ScrollFrame() override { disown(welcome); if (current_ != nullptr) disown(*current_); }

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
        welcome.layout({0, 0, area.w, area.h});
    }
    void render(tui::Surface& surface) override {
        if (welcome.visible) {
            if (current_) surface.fill({0, 0, surface.cols(), surface.rows()}, U' ', current_->theme().background);
            auto view = surface.view(welcome.rect());
            welcome.render(view); welcome.clear_dirty(); clear_dirty(); return;
        }
        if (current_ == nullptr) return;
        // 提示独占滚动区下方一行；先按完整视口判断，避免提示自身制造未读行。
        auto& doc = current_->document();
        doc.begin_frame(surface.cols(), current_->theme().epoch);
        const auto anchor = doc.anchor();
        const auto top = anchor.pinned_to_bottom ? doc.total_rows()
            : doc.row_of(anchor.block_id, anchor.byte_in_block, current_->theme()).value_or(0);
        const bool hint = !anchor.pinned_to_bottom && surface.rows() > 1 &&
            doc.total_rows() > top + static_cast<size_t>(surface.rows());
        const int height = surface.rows() - (hint ? 1 : 0);
        if (current_->rect().h != height) current_->layout({0, 0, surface.cols(), height});
        auto content = surface.view({0, 0, surface.cols(), height});
        current_->render(content); current_->clear_dirty();
        if (hint) {
            surface.fill({0, height, surface.cols(), 1}, U' ', current_->theme().background);
            const int width = reading_width(surface.cols());
            const auto text = fit_columns(format_text(ui::text().card_unseen, current_->unseen_rows()), width);
            const int x = (surface.cols() - width) / 2 + std::max(0, width - display_width(text));
            surface.text(x, height, text, current_->theme().text_faint);
        }
        clear_dirty();
    }
    bool dirty_tree() const noexcept override {
        return dirty_ || (welcome.visible ? welcome.dirty_tree() : (current_ != nullptr && current_->dirty_tree()));
    }
    bool needs_layout() const noexcept override {
        return layout_dirty_ || (current_ != nullptr && current_->needs_layout());
    }
    void invalidate_tree() noexcept override {
        invalidate(); welcome.invalidate_tree();
        if (current_ != nullptr) current_->invalidate_tree();
    }
    void invalidate_rect(tui::Rect rect) override {
        if (current_ != nullptr) current_->invalidate_rect(rect);
        Widget::invalidate_rect(rect);
    }
    tui::Widget* hit_test(tui::Point point) const noexcept override {
        if (welcome.visible) return welcome.hit_test(point);
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
    const std::string value = base::path_to_utf8(path);
    if (const auto home = base::home_directory()) {
        const std::string& prefix = *home;
        if (value == prefix) return "~";
        if (value.starts_with(prefix + "/")) return "~" + value.substr(prefix.size());
    }
    return value;
}

std::string relative_age(std::chrono::system_clock::time_point when) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now() - when)
                             .count();
    if (seconds < 60) return std::string(ui::text().panel_just_now);
    if (seconds < 3600) return format_text(ui::text().panel_minutes, seconds / 60);
    if (seconds < 86400) return format_text(ui::text().panel_hours, seconds / 3600);
    return format_text(ui::text().panel_days, seconds / 86400);
}

} // namespace

/// @brief 前端投影：只持 Client、只读 DTO 与页面状态，不持执行对象，不写业务状态。
class Shell final : public tui::EventHandler {
public:
    Shell(client::Client& client, FrontendBridge& bridge, const protocol::SessionSnapshot& initial,
          std::vector<protocol::HistoryItem> history,
          const InteractiveOptions& options, std::filesystem::path project_root, std::string branch)
        : client_(client), bridge_(bridge), appearance_(Appearance::from_json(options.appearance)), layout_(Layout::from_json(options.layout)),
          themes_directory_(options.themes_directory),
          resumed_(options.resumed), history_(std::move(history)),
          initial_prompt_(options.initial_prompt), root_(layout()), rt_(terminal_, root_),
          prompt_(*input_, [this](std::string text) { submit(std::move(text)); },
                  [this] { recall(); }, [this] { prompt_changed(); },
                  [this] { return completion_.visible(); }),
          dialog_(rt_, [this] { interrupt(); }), model_dialog_(rt_),
          toasts_(rt_), panel_(rt_),
          memory_browser_(panel_, {
              [this](std::string method, nlohmann::json params, MemoryBrowser::Done done, MemoryBrowser::Failed failed) {
                  request(std::move(method), std::move(params), std::move(done), std::move(failed));
              },
              [this](std::string message, tui::Notice::Severity severity) { toast(std::move(message), severity); },
              [this] { return terminal_.size().cols; }
          }),
          settings_(rt_),
          model_picker_(rt_, {
              [this](std::string method, nlohmann::json params, ModelPicker::Done done, ModelPicker::Failed failed) {
                  request(std::move(method), std::move(params), std::move(done), std::move(failed));
              },
              [this] { return target(); },
              [this](const std::string& name) { switch_model(name); },
              [this] { add_model(); },
              [this] { refresh_models(); refresh_snapshot(); },
              [this](ToastStack::Severity severity, std::string message) { toasts_.show(severity, std::move(message)); }
          }),
          completion_(rt_, *input_, {
              [this](std::string method, nlohmann::json params, CompletionController::Done done) {
                  request(std::move(method), std::move(params), std::move(done));
              },
              [this] { return id_; },
              [this] {
                  std::vector<CompletionController::Command> commands;
                  for (const auto& meta : command_ui_) {
                      const auto command = std::ranges::find_if(keys_.commands(), [&](const tui::Command& item) {
                          return item.id == meta.id;
                      });
                      commands.push_back({meta.alias, command == keys_.commands().end() ? "" : command->title});
                  }
                  return commands;
              },
              [this](std::string text) { submit(std::move(text)); }
          }),
          branch_(std::move(branch)), project_root_(std::move(project_root)) {
        apply_snapshot(initial);
        project_path_ = display_path(project_root_);
        hints_->path = project_path_;
        side_->set_project_root(project_root_);
        header_->branch = branch_;
        hints_->hints = [this] { return key_hints(); };
        side_->set_version(DAGENT_VERSION);
        add_pane("main", id_, {});
        update_prompt_footer();
        rt_.set_focus(&prompt_, input_); rt_.set_global(*this);
        terminal_.set_mouse(true); // 不开这一行，滚轮与拖选的转义序列根本不会上报
        bind_active_mouse();
        tabs_->select = [this](size_t i) { show_pane(i); };
        rt_.bind_mouse(*tabs_, *tabs_);
        rt_.bind_mouse(frame_->welcome, frame_->welcome);
        prompt_.before_edit = [this](const tui::Event& e) { return welcome_event(e); };
        workspace_->resize = [this](int columns) { update_panels(columns); };
        navigator_->source = side_; side_->mirror = navigator_;
        navigator_->back = [this] { focus_prompt(); };
        navigator_->focus = [this] { focus_navigator(); };
        side_->focus = [this] { focus_prompt(); };
        side_->all_sessions = [this] { focus_prompt(); session_panel(); };
        rt_.bind_mouse(*navigator_, *navigator_); rt_.bind_mouse(*side_, *side_);
        register_commands();
        apply_layout(layout_);
        apply_appearance(appearance_);
        rt_.on_caps([this](const tui::Terminal::Caps&) {
            if (appearance_.mode == "auto") apply_appearance(appearance_);
        });
        refresh_models();
        reset_transcript(resumed_, history_.size());
        for (const auto& item : history_) active_transcript().append_history(item);
        history_.clear();
        refresh_agents();
        refresh_project();
        refresh_navigation();
        refresh_mcp();
    }

    ~Shell() override {
        cancel_history();
        for (auto& pane : panes_) if (pane.pager) pane.pager->cancel();
        {
            const std::lock_guard lock(gate_->mutex);
            gate_->open = false;
        }
        model_picker_.close();
        rt_.cancel(git_debounce_);
        terminal_.restore();
        unbind_active_mouse();
        rt_.unbind_mouse(*navigator_); rt_.unbind_mouse(*side_);
        rt_.unbind_mouse(*tabs_);
        rt_.unbind_mouse(frame_->welcome);
    }

    int run(std::stop_token stop) {
        std::stop_callback signal(stop, [this] {
            rt_.post([this] { signal_exit_ = true; exit(); });
        });
        if (!initial_prompt_.empty()) submit(initial_prompt_);
        rt_.run();
        terminal_.restore();
        exiting_ = true;
        model_picker_.close();
        cancel_history();
        for (auto& pane : panes_) if (pane.pager) pane.pager->cancel();
        if (client_.connected()) {
            try {
                client_.call("backend.shutdown");
            } catch (const std::exception&) {
                // 后端已在收尾或连接已关闭；宽限回收由 launcher 负责。
            }
        }
        bridge_.detach();
        if (!error_.empty()) std::cerr << "failed: " << error_ << '\n';
        std::cout << std::format("Session {0} saved. Resume with: dagent -r {0}\n", id_);
        return !error_.empty() ? 1 : signal_exit_ ? 130 : 0;
    }

    // ---- FrontendBridge：后端线程 → 渲染线程（Shell 已 detach 后不再到达）----
    void on_backend_event(const protocol::Event& event) {
        rt_.post([this, event] { if (!exiting_) handle_event(event); });
    }
    void on_backend_interaction(const protocol::InteractionRequest& request) {
        rt_.post([this, request] { if (!exiting_) show_interaction(request); });
    }
    void on_backend_interaction_closed(const std::string& interaction_id) {
        rt_.post([this, interaction_id] {
            if (exiting_) return;
            if (active_interaction_ == interaction_id) {
                active_interaction_.clear();
                dialog_.close();
            }
        });
    }
    void on_backend_disconnected(const std::string& message) {
        rt_.post([this, message] {
            if (exiting_) return;
            error_ = message;
            exit();
        });
    }

    bool on_event(const tui::Event& event) override {
        if (event.kind == tui::Event::Kind::focus && event.focus_gained) { schedule_git(); refresh_navigation_sessions(); }
        if (event.kind == tui::Event::Kind::resize) {
            update_panels(event.size.cols);
        }
        return keys_.on_event(event);
    }

private:
    struct CommandUi { std::string id, shortcut, alias; };

    // ---- RPC：请求在读取线程返回，回调经 rt.post 回到渲染线程 ----
    nlohmann::json target() const {
        return {{"session_id", id_}, {"session_generation", generation_}};
    }
    void request(std::string method, nlohmann::json params,
                 std::function<void(const nlohmann::json&)> done,
                 std::function<void(const protocol::RpcError&)> failed = {}) {
        auto gate = gate_;
        client_.call_async(std::move(method), std::move(params),
                           [this, gate, done = std::move(done), failed = std::move(failed)](
                               std::expected<nlohmann::json, protocol::RpcError> result) {
            const std::lock_guard lock(gate->mutex);
            if (!gate->open) return;
            if (!result) {
                if (failed) {
                    rt_.post([failed = std::move(failed), error = std::move(result.error())] {
                        failed(error);
                    });
                }
                return;
            }
            if (done) {
                rt_.post([done = std::move(done), value = std::move(*result)]() mutable {
                    done(value);
                });
            }
        });
    }
    /// @brief 命令的失败处理：解除本地 pending；执行期失败已有 notice 事件，不重复提示。
    std::function<void(const protocol::RpcError&)> command_failure() {
        return [this](const protocol::RpcError& error) {
            set_busy(false);
            if (error.kind == protocol::error_kind::kInvalidState ||
                error.kind == protocol::error_kind::kConfigError) {
                return;
            }
            toast(error.message, tui::Notice::Severity::warn);
        };
    }
    void refresh_snapshot() {
        request("session.snapshot", target(), [this](const nlohmann::json& value) {
            apply_snapshot(value.get<protocol::SessionSnapshot>());
        });
    }

    void update_context(std::size_t used, std::size_t limit) {
        header_->used = used; header_->limit = limit; header_->invalidate();
        side_->set_context(used, limit);
    }

    void apply_snapshot(const protocol::SessionSnapshot& snapshot) {
        if (snapshot.session_generation < generation_ || snapshot.state_seq < state_seq_) return;
        state_seq_ = snapshot.state_seq;
        snapshot_ = snapshot;
        id_ = snapshot.session_id;
        generation_ = snapshot.session_generation;
        model_name_ = snapshot.model.name;
        model_label_ = snapshot.model.model;
        model_picker_.update_models(models_, model_name_);
        planning_ = snapshot.planning;
        mode_ = snapshot.permission_mode;
        read_only_ = snapshot.read_only;
        window_tokens_ = snapshot.context.window;
        mcp_states_ = decode_mcp(snapshot.mcp);
        side_->set_mcp(mcp_states_);
        update_welcome();
        header_->trigger = snapshot.context.trigger_percent;
        side_->set_trigger(snapshot.context.trigger_percent);
        update_context(snapshot.context.used, snapshot.context.limit);
        if (!panes_.empty()) {
            panes_.front().transcript->set_session(mode_label(), model_label_);
            update_prompt_footer();
        }
        set_busy(snapshot.busy);
        if (snapshot.current_operation && snapshot.current_operation->kind == "compact")
            turn_meter_.set_phase(std::string(ui::text().act_compacting));
        refresh_queue();
        refresh_mcp();
        update_todo_status(terminal_.size().cols);
        maybe_prompt_permission(snapshot);
    }

    void handle_event(const protocol::Event& event) {
        if (event.kind == "workspace.changed") { schedule_git(); return; }
        if (event.kind == "session.changed") {
            apply_session_changed(event);
            return;
        }
        if (event.session_generation != 0 && event.session_generation != generation_) return;
        if (event.kind == "operation.finished") {
            if (event.data.value("status", "done") == "interrupted")
                toast(std::string(ui::text().toast_compact_cancelled));
            set_busy(false);
            return;
        }
        if (event.kind == "tool_finished" || event.kind == "turn_ended") schedule_git();
        if (event.kind == "turn_started" || event.kind == "turn_ended") refresh_navigation_sessions();
        if (event.parent_session_id) {
            route_sub_event(event);
            return;
        }
        if (event.session_id != id_) return;
        const bool update_state = event.seq > state_seq_;
        state_seq_ = std::max(state_seq_, event.seq);
        if (auto decoded = decode_event(event)) {
            if (auto* started = std::get_if<TurnStarted>(&decoded->payload)) {
                started->time = local_time();
                const auto submitted = std::ranges::find_if(submitted_, [&](const auto& item) { return item.text == started->input; });
                if (submitted != submitted_.end()) { started->time = submitted->time; submitted_.erase(submitted); }
            }
            apply_live(*decoded, update_state);
        }
    }

    void apply_session_changed(const protocol::Event& event) {
        if (event.session_generation < generation_) return;
        cancel_history();
        if (permission_prompt_) {
            permission_prompt_ = false;
            if (permission_dialog_open_) { dialog_.close(); permission_dialog_open_ = false; }
        }
        id_ = event.session_id;
        generation_ = event.session_generation;
        const bool replace = event.data.value("replace_transcript", true);
        const bool resumed = event.data.value("resumed", false);
        refresh_snapshot();
        if (replace) {
            reset_transcript(resumed, 0);
            side_->set_title({});
            if (resumed) refresh_history();
        } else {
            panes_.front().transcript->set_session(mode_label(), model_label_);
            update_prompt_footer();
        }
        set_busy(false);
        refresh_project();
        refresh_navigation_sessions();
        refresh_models();
    }

    void route_sub_event(const protocol::Event& event) {
        const std::string call_id = event.model_call_id.value_or("");
        const std::string agent = event.agent.value_or("");
        std::size_t index = 0;
        if (const auto it = task_panes_.find(call_id); it != task_panes_.end()) {
            index = it->second;
        } else {
            index = add_pane("task · " + agent, event.session_id, call_id);
            task_panes_.emplace(call_id, index);
        }
        if (index == 0) return;
        auto decoded = decode_event(event);
        if (!decoded) return;
        if (auto* started = std::get_if<TurnStarted>(&decoded->payload)) {
            started->time = local_time(); panes_[index].description = started->input;
        }
        panes_[index].transcript->apply_live(*decoded);
        panes_.front().transcript->apply_child(call_id, *decoded, *panes_[index].transcript);
        if (const auto* ended = std::get_if<TurnEnded>(&decoded->payload)) {
            panes_[index].state = ended->status == TurnStatus::done ? AgentState::done : AgentState::failed;
            panes_[index].seconds = std::chrono::duration<double>(Clock::now() - panes_[index].began).count();
        }
        refresh_agents();
    }

    void apply_live(const Event& event, bool update_state) {
        if (std::holds_alternative<TurnStarted>(event.payload)) hide_welcome();
        panes_.front().transcript->apply_live(event);
        std::visit(base::Overloaded{
                       [&](const TurnStarted&) {
                           turn_meter_.start_turn();
                           if (update_state) { set_busy(true); refresh_snapshot(); }
                       },
                       [&](const StepStarted&) { turn_meter_.start_step(); },
                       [&](const TextDelta& e) { turn_meter_.streamed(e.text); },
                       [&](const ReasoningDelta& e) { turn_meter_.streamed(e.text); },
                       [&](const ToolPending&) { turn_meter_.tool_pending(); },
                       [&](const StreamReset&) { turn_meter_.reset_stream(); },
                       [&](const ToolFinished& e) {
                           for (auto& pane : panes_) if (pane.call_id == e.id) {
                               pane.state = e.is_error || e.interrupted ? AgentState::failed : AgentState::done;
                               pane.seconds = std::chrono::duration<double>(Clock::now() - pane.began).count();
                           }
                           refresh_agents();
                       },
                       [&](const ContextUpdate& e) {
                           turn_meter_.complete_call(e.output);
                           if (!update_state) return;
                           update_context(e.used, e.limit);
                       },
                       [&](const Retrying& e) {
                           turn_meter_.retry();
                           toast(format_text(ui::text().toast_retry, e.reason, e.wait_ms / 1000.0,
                                             e.attempt, e.max_attempts),
                                 tui::Notice::Severity::warn);
                       },
                       [&](const Notice& e) {
                           if (e.level != NoticeLevel::error)
                               toast(e.text, e.level == NoticeLevel::warn
                                                 ? tui::Notice::Severity::warn
                                                 : tui::Notice::Severity::info);
                       },
                       [&](const ModeChanged& e) {
                           if (!update_state) return;
                           planning_ = e.planning;
                           mode_ = e.mode;
                           panes_.front().transcript->set_session(mode_label(), model_label_);
                           update_prompt_footer();
                       },
                       [&](const TurnEnded& e) {
                           for (size_t i = 1; i < panes_.size(); ++i) if (panes_[i].state == AgentState::running)
                               panes_[i].transcript->apply_live(Event{TurnEnded{TurnStatus::interrupted, {}, {}}});
                           panes_[0].state = e.status == TurnStatus::done ? AgentState::done : AgentState::failed;
                           if (!update_state) return;
                           dialog_.close();
                           set_busy(false);
                           refresh_snapshot();
                       },
                       [](const auto&) {}
                   },
                   event.payload);
        activity();
    }

    void set_busy(bool value) {
        if (busy_ == value) return;
        busy_ = value; rt_.cancel(activity_timer_); activity_timer_ = 0;
        input_->set_active(!value && active_ == 0); update_prompt_footer();
        if (value) {
            turn_meter_.start_activity();
            activity_timer_ = rt_.every(100ms, [this] {
                activity(); activity_->tick(); animation_ = (animation_ + 1) % icons().spinner.size();
                for (auto& pane : panes_) pane.transcript->tick();
                refresh_agents(); return true;
            });
        } else {
            for (auto& pane : panes_) if (pane.state == AgentState::running) {
                pane.state = AgentState::failed;
                pane.seconds = std::chrono::duration<double>(Clock::now() - pane.began).count();
            }
        }
        refresh_agents(); refresh_mcp(); activity();
    }

    void cancel_history() {
        if (history_pager_) history_pager_->cancel();
        history_pager_.reset();
    }

    void refresh_history() {
        cancel_history();
        const std::string id = id_;
        const auto generation = generation_;
        auto items = std::make_shared<std::vector<protocol::HistoryItem>>();
        auto pager = std::make_shared<HistoryPager>(client_, rt_, gate_, id);
        pager->on_item = [items](const protocol::HistoryItem& item) { items->push_back(item); };
        pager->on_done = [this, id, generation, items] {
            if (id_ != id || generation_ != generation || exiting_) return;
            reset_transcript(true, items->size());
            for (const auto& item : *items) panes_.front().transcript->append_history(item);
            history_pager_.reset();
            refresh_agents();
            root_.invalidate_tree();
            refresh_mcp();
        };
        pager->on_error = [this, id, generation](std::string message) {
            if (id_ != id || generation_ != generation || exiting_) return;
            history_pager_.reset();
            toast(std::string(ui::text().toast_resume_failed) + message, tui::Notice::Severity::error);
        };
        history_pager_ = pager;
        pager->start();
    }

    void refresh_project() {
        schedule_git();
        const std::string id = id_;
        request("workspace.info", {{"session_id", id}}, [this, id](const nlohmann::json& value) {
            if (id_ != id) return;
            branch_ = value.value("branch", "");
            header_->branch = branch_; header_->invalidate();
            update_welcome();
        });
    }

    void focus_prompt() {
        navigator_focused_ = false; navigator_->set_focused(false);
        rt_.set_focus(&prompt_, input_); hints_->invalidate();
    }
    void focus_navigator() {
        completion_.close(); navigator_focused_ = true; navigator_->set_focused(true);
        rt_.set_focus(navigator_, navigator_); hints_->invalidate();
    }
    void update_panels(int columns) {
        const bool status = !sidebar_override_.value_or(layout_.status_panel == "hidden") && columns >= 80;
        const int width = columns >= 100 ? 30 : 26;
        const bool wanted = navigator_override_.value_or(layout_.navigator == "always" || (layout_.navigator == "auto" && columns >= 170));
        const bool navigator = wanted && columns - (status ? width : 0) - width >= 50;
        side_->columns = navigator_->columns = width;
        side_->set_collapsed(!status); navigator_->set_collapsed(!navigator);
        auto& welcome = frame_->welcome;
        const bool sessions_in_navigator = navigator &&
            std::ranges::find(navigator_->order, "sessions") != navigator_->order.end();
        if (welcome.sessions_in_navigator != sessions_in_navigator) {
            welcome.sessions_in_navigator = sessions_in_navigator;
            welcome.selected = -1; welcome.reveal_selection = false; welcome.invalidate();
            hints_->invalidate();
        }
        side_->left = layout_.status_panel == "left"; navigator_->left = !side_->left;
        workspace_->left = side_->left; workspace_->invalidate_layout();
        if (!navigator && navigator_focused_) focus_prompt();
        update_todo_status(columns);
    }
    void apply_layout(const Layout& value) {
        if (value.status_panel != layout_.status_panel) sidebar_override_.reset();
        if (value.navigator != layout_.navigator) navigator_override_.reset();
        layout_ = value;
        const int width = value.width.is_number_integer() ? value.width.get<int>() : value.width == "compact" ? 88 : value.width == "wide" ? 160 : 0;
        set_reading_width(width);
        side_->order = value.status_sections; navigator_->order = value.navigator_sections;
        update_panels(terminal_.size().cols);
        // Width participates in document measurement, so invalidate materialized lines via the theme epoch.
        apply_theme(theme_); root_.invalidate_layout();
    }
    void toggle_navigator() {
        if (navigator_focused_) {
            navigator_override_ = false; focus_prompt();
        } else {
            navigator_override_ = true;
            update_panels(terminal_.size().cols);
            if (!navigator_->collapsed()) focus_navigator();
        }
        update_panels(terminal_.size().cols);
        refresh_navigation_sessions(); schedule_git();
    }
    void refresh_navigation_sessions() {
        const auto id = id_;
        request("session.list", {{"limit", 0}}, [this, id](const nlohmann::json& value) {
            if (id != id_) return;
            SidePanel::Section section;
            const auto sessions = value.value("sessions", nlohmann::json::array());
            for (const auto& session : sessions) {
                const auto session_id = session.value("id", "");
                const auto title = session.value("title", "");
                if (title.empty() && session_id != id) continue;
                const auto age = relative_age(std::chrono::system_clock::time_point(std::chrono::milliseconds(session.value("updated", std::int64_t{0}))));
                section.rows.push_back({title.empty() ? std::string(ui::text().new_session) : title, age,
                    [this, session_id] { focus_prompt(); if (session_id != id_) resume_session(session_id); }, session_id == id_, 0});
            }
            section.right = std::to_string(section.rows.size());
            section.rows.push_back({std::string(ui::text().navigator_new_session), {}, [this] { focus_prompt(); new_session(); }, false, 0});
            side_->sections["sessions"] = std::move(section); side_->invalidate();
        });
    }
    void schedule_git() {
        rt_.cancel(git_debounce_);
        git_debounce_ = rt_.after(std::chrono::milliseconds(120), [this] {
            const auto id = id_;
            request("workspace.git_status", {{"session_id", id}}, [this, id](const nlohmann::json& value) {
                if (id != id_) return;
                SidePanel::Section section;
                if (!value.value("repository", false)) section.rows.push_back({std::string(ui::text().navigator_no_repository), {}, {}, false, 'f'});
                else {
                    const auto branch = value.value("branch", "");
                    section.right = std::format("↑{} ↓{}", value.value("ahead", 0), value.value("behind", 0));
                    section.rows.push_back({std::string(icons().branch) + " " + branch, {}, {}, false, 0});
                    const auto files = value.value("files", nlohmann::json::array());
                    std::map<std::string, int> counts;
                    int shown = 0;
                    for (const auto& file : files) {
                        const auto status = file.value("status", "?"); ++counts[status];
                        if (shown++ >= 8) continue;
                        section.rows.push_back({status + " " + file.value("path", ""),
                            std::format("+{} −{}", file.value("added", 0), file.value("removed", 0)), {}, false, status.front()});
                    }
                    if (files.size() > 8) section.rows.push_back({"+" + std::to_string(files.size() - 8) + " more", {}, {}, false, 'f'});
                    std::string summary;
                    for (const auto& [status, count] : counts) {
                        if (!summary.empty()) summary += " · ";
                        summary += std::to_string(count) + " " + (std::string(status == "?" ? ui::text().navigator_untracked : status == "A" ? ui::text().navigator_added : status == "D" ? ui::text().navigator_deleted : ui::text().navigator_modified));
                    }
                    section.rows.push_back({summary.empty() ? std::string(ui::text().navigator_clean) : summary, {}, {}, false, 'f'});
                    branch_ = branch + (files.empty() ? "" : "*"); header_->branch = branch_; header_->invalidate(); update_welcome();
                }
                side_->sections["git"] = std::move(section); side_->invalidate();
            });
        });
    }
    void refresh_navigation() {
        refresh_navigation_sessions();
        request("skills.list", {}, [this](const nlohmann::json& value) {
            SidePanel::Section section;
            for (const auto& skill : value.value("skills", nlohmann::json::array())) {
                const auto name = skill.value("name", "");
                section.rows.push_back({"$ " + name, {}, [this, name] {
                    auto input = input_->text(); if (!input.empty() && input.back() != ' ') input += ' ';
                    input_->set_text({}); input_->insert(input + "$" + name + " "); focus_prompt();
                }, false, 0});
            }
            side_->sections["skills"] = std::move(section); side_->invalidate();
        });
        request("agents.list", {}, [this](const nlohmann::json& value) {
            SidePanel::Section section;
            for (const auto& agent : value) {
                const auto name = agent.value("name", ""), description = agent.value("description", "");
                auto summary = agent.value("permission", "inherit");
                if (summary == "read_only") summary = "read-only";
                else if (agent.contains("tools") && agent["tools"].is_array()) {
                    summary.clear();
                    const auto tools = agent["tools"].get<std::vector<std::string>>();
                    const auto has = [&](std::string_view tool) { return std::ranges::find(tools, tool) != tools.end(); };
                    if (has("edit") || has("write")) summary = ui::text().navigator_edit;
                    else if (has("grep") || has("glob")) summary = ui::text().navigator_search;
                    else if (has("read")) summary = ui::text().navigator_read;
                    if (has("bash")) { if (!summary.empty()) summary += " · "; summary += ui::text().navigator_shell; }
                    if (summary.empty()) summary = tools.empty() ? std::string(ui::text().navigator_no_tools) : tools.front();
                    if (agent.value("permission", "inherit") == "ask") summary = "ask · " + summary;
                }
                section.rows.push_back({name, summary, [this, name, description] { toast(name + ": " + description); }, false, 0});
            }
            side_->sections["agents"] = std::move(section); side_->invalidate();
        });
    }

    void update_welcome() {
        auto& view = frame_->welcome;
        view.path = project_path_; view.branch = branch_; view.model = model_label_;
        view.mode = mode_label(); view.mcp = mcp_states_; view.invalidate();
    }
    void hide_welcome() {
        if (!frame_->welcome.visible) return;
        frame_->welcome.visible = false; frame_->welcome.selected = -1;
        input_->set_welcome(false);
        frame_->invalidate_tree();
    }
    bool welcome_navigation() const {
        return frame_->welcome.visible && frame_->welcome.has_recent() &&
               input_->text().empty() && snapshot_.queue.empty() && !busy_ && active_ == 0;
    }
    bool welcome_event(const tui::Event& e) {
        auto& view = frame_->welcome;
        if (view.visible && input_->text().empty() && e.mods == tui::Mods::none) {
            if (e.key == tui::Key::page_up || e.key == tui::Key::page_down) {
                view.scroll((e.key == tui::Key::page_up ? -1 : 1) * std::max(1, view.rect().h - 1)); return true;
            }
            if (e.key == tui::Key::home || e.key == tui::Key::end) {
                view.top = e.key == tui::Key::home ? 0 : view.content_rows;
                view.reveal_selection = false; view.invalidate(); return true;
            }
        }
        if (welcome_navigation() && !completion_.visible() && e.mods == tui::Mods::none) {
            const int count = static_cast<int>(view.sessions.size());
            if (e.key == tui::Key::up || e.key == tui::Key::down) {
                view.selected = view.selected < 0 ? (e.key == tui::Key::up ? count - 1 : 0)
                    : (view.selected + (e.key == tui::Key::up ? count - 1 : 1)) % count;
                view.reveal_selection = true; view.invalidate(); return true;
            }
            if (e.key == tui::Key::enter && view.selected >= 0) {
                resume_session(view.sessions[view.selected].id); return true;
            }
        }
        if (view.selected >= 0 && (e.kind == tui::Event::Kind::text || e.kind == tui::Event::Kind::paste)) {
            view.selected = -1; view.invalidate();
        }
        return false;
    }
    void refresh_welcome() {
        const auto id = id_;
        request("session.list", {{"limit", 0}}, [this, id](const nlohmann::json& value) {
            auto& view = frame_->welcome;
            if (id != id_ || !view.visible) return;
            view.sessions.clear();
            for (const auto& session : value.value("sessions", nlohmann::json::array())) {
                const auto title = session.value("title", "");
                if (session.value("id", "") == id || title.empty() || view.sessions.size() == 5) continue;
                const auto updated = std::chrono::system_clock::time_point(std::chrono::milliseconds(session.value("updated", std::int64_t{0})));
                auto age = relative_age(updated);
                const auto now = std::time(nullptr), then = std::chrono::system_clock::to_time_t(updated);
                std::tm today{}, previous{}; base::local_time(now, today); base::local_time(then, previous);
                auto midnight = today; midnight.tm_hour = midnight.tm_min = midnight.tm_sec = 0;
                const auto today_start = std::mktime(&midnight);
                --midnight.tm_mday; midnight.tm_isdst = -1;
                const auto yesterday_start = std::mktime(&midnight);
                if (then < today_start && then >= yesterday_start) age = ui::text().welcome_yesterday;
                else if (then < yesterday_start) {
                    age = format_text(ui::text().welcome_date, ui::text().welcome_months.substr(previous.tm_mon * 3, 3), previous.tm_mday);
                }
                view.sessions.push_back({session.value("id", ""), title, age});
            }
            view.selected = -1; view.invalidate();
        });
        request("memory.list", {}, [this, id](const nlohmann::json& value) {
            if (id != id_ || !frame_->welcome.visible) return;
            frame_->welcome.memories = value.value("entries", nlohmann::json::array()).size();
            frame_->welcome.invalidate();
        });
    }

    void refresh_models() {
        request("model.list", nlohmann::json::object(), [this](const nlohmann::json& value) {
            models_.clear();
            for (const auto& model : value.value("models", nlohmann::json::array()))
                models_.push_back(model.get<protocol::PublicModel>());
            model_picker_.update_models(models_, model_name_);
            provider_kinds_.clear();
            for (const auto& kind : value.value("provider_kinds", nlohmann::json::array())) {
                const protocol::ProviderKind info = kind.get<protocol::ProviderKind>();
                provider_kinds_.push_back({info.kind, info.default_base_url, info.needs_credential});
            }
        });
    }

    void refresh_queue() { queue_label_->set_queue(snapshot_.queue); }

    bool mcp_pending() const {
        return std::ranges::any_of(mcp_states_, [](const McpStatus& state) {
            return state.status == "connecting" || state.status == "reconnecting";
        });
    }
    void refresh_mcp() {
        side_->set_mcp(mcp_states_);
        if (!mcp_pending() && !busy_) return;
        if (mcp_timer_) return;
        mcp_timer_ = rt_.every(200ms, [this] {
            refresh_snapshot();
            if (mcp_pending() || busy_) return true;
            mcp_timer_ = 0; return false;
        });
    }
    static std::string mcp_label(const std::vector<McpStatus>& states) {
        if (states.empty()) return {};
        const auto ready = std::ranges::count_if(states, [](const McpStatus& state) {
            return state.status == "ready";
        });
        const auto remote = std::ranges::count_if(states, [](const McpStatus& state) {
            return state.status == "ready" && state.boundary == "remote";
        });
        if (remote > 0) return format_text(ui::text().status_mcp_remote, ready, states.size(), remote);
        return format_text(ui::text().status_mcp, ready, states.size());
    }

    void toast(std::string text, tui::Notice::Severity severity = tui::Notice::Severity::info) {
        toasts_.show(severity, std::move(text));
    }
    void apply_theme(const tui::ThemeTokens& selected) {
        theme_ = resolve_theme(selected); theme_.epoch = ++theme_epoch_;
        for (Pane& pane : panes_) pane.scroll->set_theme(theme_);
        activity_->set_theme(theme_);
        queue_label_->set_theme(theme_); tabs_->set_theme(theme_);
        input_->set_theme(theme_); header_->set_theme(theme_); hints_->set_theme(theme_);
        frame_->welcome.set_theme(theme_);
        navigator_->set_theme(theme_); side_->set_theme(theme_); toasts_.set_theme(theme_); panel_.set_theme(theme_);
        dialog_.set_theme(theme_); model_dialog_.set_theme(theme_); completion_.set_theme(theme_);
        settings_.set_theme(theme_);
        model_picker_.set_theme(theme_, appearance_.panels == "outline");
        root_.invalidate_tree();
    }
    void activity() {
        if (!busy_) { activity_->set_action({}); return; }
        const auto meter = turn_meter_.display();
        activity_->set_action(format_text(ui::text().activity_elapsed,
            dialog_.active() ? std::string(interaction_kind_ == "question" && !permission_dialog_open_
                ? ui::text().act_waiting_answer : ui::text().act_waiting) : turn_meter_.phase(), meter.seconds, icons().separator),
            meter.tokens, meter.rate);
    }

    std::unique_ptr<tui::Widget> layout() {
        auto main = std::make_unique<tui::Container>();
        // 输入区（活动行、排队行、输入框）横跨主区域，正文仍在阅读栏内。
        auto add_spanning = [&]<class T>(T*& target, tui::Constraint constraint) {
            auto widget = std::make_unique<T>(); target = widget.get();
            main->add(constraint, spanning(std::move(widget), 2, theme_));
        };
        auto frame = std::make_unique<ScrollFrame>(); frame_ = frame.get();
        main->add({tui::Sizing::flex, 1}, std::move(frame));
        add_spanning(activity_, {tui::Sizing::content});
        add_spanning(queue_label_, {tui::Sizing::content});
        add_spanning(input_, {tui::Sizing::content, 0, 2, PromptBox::k_max_rows + 5});

        auto workspace = std::make_unique<Workspace>(); workspace_ = workspace.get();
        workspace->add({tui::Sizing::flex, 1}, std::move(main));
        auto side = std::make_unique<SidePanel>(); side_ = side.get();
        workspace->add({tui::Sizing::content}, std::move(side));
        auto navigator = std::make_unique<SidePanel>(); navigator_ = navigator.get();
        workspace->add({tui::Sizing::content}, std::move(navigator));

        auto page = std::make_unique<Page>();
        page->modal = [this] { return model_picker_.active() || panel_.visible() || settings_.active() || dialog_.active() || model_dialog_.active(); };
        page->dim = [this](tui::Surface& surface) { dim_surface(surface, theme_, terminal_.caps().background); };
        auto header = std::make_unique<HeaderBar>(); header_ = header.get();
        page->add({tui::Sizing::content}, std::move(header));
        auto tabs = std::make_unique<AgentTabs>(); tabs_ = tabs.get();
        page->add({tui::Sizing::content}, std::move(tabs));
        page->add({tui::Sizing::flex, 1}, std::move(workspace));
        auto hints = std::make_unique<HintBar>(); hints_ = hints.get();
        page->add({tui::Sizing::fixed, 1}, std::move(hints));
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
        add_command("memory.manage", "View and delete memories", std::string(ui::text().cmd_session), {}, "/memory", [this] { memory_browser_.open(); });
        add_command("skill.pick", std::string(ui::text().cmd_skills), std::string(ui::text().cmd_session), {}, "/skills", [this] { skill_panel(); });
        add_command("panel.commands", std::string(ui::text().cmd_palette), std::string(ui::text().cmd_view), "ctrl+p", {}, [this] { command_panel(); });
        add_command("session.interrupt", std::string(ui::text().cmd_interrupt), std::string(ui::text().cmd_session), "escape", {}, [this] { interrupt(); });
        add_command("session.cancel", std::string(ui::text().cmd_cancel), std::string(ui::text().cmd_session), "ctrl+c", {}, [this] { cancel(); });
        add_command("permission.cycle", std::string(ui::text().cmd_permission), std::string(ui::text().cmd_permission_group), "shift+tab", {}, [this] { cycle_permission(); });
        add_command("permission.list", "Session permissions", std::string(ui::text().cmd_permission_group), {}, "/permissions", [this] { permissions_panel(); });
        add_command("mode.plan", "Toggle planning mode", std::string(ui::text().cmd_permission_group), "ctrl+g", "/plan", [this] { toggle_plan(); });
        add_command("tools.expand", std::string(ui::text().cmd_tools), std::string(ui::text().cmd_transcript), "ctrl+o", {}, [this] { active_transcript().toggle_tools(); });
        add_command("diff.toggle", std::string(ui::text().cmd_diff), std::string(ui::text().cmd_transcript), "ctrl+d", {}, [this] {
            unified_diff_ = !unified_diff_;
            for (auto& pane : panes_)
                pane.transcript->set_render_options(render_options());
        }, [this] { return active_transcript().can_switch_diff(active_scroll(), false); });
        add_command("thoughts.toggle", std::string(ui::text().cmd_thoughts), std::string(ui::text().cmd_transcript), "ctrl+r", {}, [this] { active_transcript().toggle_thoughts(); });
        add_command("navigator.toggle", std::string(ui::text().settings_navigator), std::string(ui::text().cmd_view), "ctrl+b", {}, [this] { toggle_navigator(); });
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
        add_command("theme.pick", std::string(ui::text().cmd_theme), std::string(ui::text().cmd_view), {}, "/theme", [this] { settings_panel(); });
        add_command("settings.show", std::string(ui::text().settings_title), std::string(ui::text().cmd_view), {}, "/settings", [this] { settings_panel(); });
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

    // ---- 子会话 Pane：索引 0 恒为主会话，task 的子 Agent 各占一个 Pane ----
    struct Pane {
        tui::Scrollback* scroll = nullptr;
        std::unique_ptr<Transcript> transcript;
        std::unique_ptr<tui::ScrollbackMouse> mouse;
        std::shared_ptr<HistoryPager> pager;
        std::string title, session_id, call_id;
        AgentState state = AgentState::running;
        Clock::time_point began = Clock::now();
        double seconds = 0;
        std::string description = {};
    };
    void refresh_agents() {
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
    tui::Scrollback& active_scroll() { return *panes_[active_].scroll; }
    Transcript& active_transcript() { return *panes_[active_].transcript; }
    void bind_active_mouse() { rt_.bind_mouse(active_scroll(), *panes_[active_].mouse); }
    void unbind_active_mouse() { rt_.unbind_mouse(active_scroll()); }

    RenderOptions render_options() const {
        return {.auto_diff = appearance_.diff == "auto",
                .unified = unified_diff_,
                .links = appearance_.links == "on",
                .project = project_root_};
    }

    std::size_t add_pane(std::string title, std::string session_id, std::string call_id) {
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
    void show_pane(std::size_t index) {
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
    void close_child_panes() {
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
    /// 恢复出来的 task 没有 Pane：切进去时再回放子会话记录。
    void open_task_pane(const Transcript::TaskRef& task) {
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
    void agent_panel() {
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

    void update_todo(const TodoList& value) {
        side_->set_items(value);
        update_todo_status(terminal_.size().cols);
    }
    void update_todo_status(int columns) {
        const bool hidden = side_->collapsed() || columns < 80;
        header_->sidebar = !hidden; header_->invalidate();
    }
    void toggle_todo() {
        sidebar_override_ = !side_->collapsed();
        update_panels(terminal_.size().cols);
    }
    /// 输入框尾行与消息尾行共用同一份「模式 · 模型」。
    std::string mode_label() const {
        if (planning_) return "plan";
        if (read_only_) return std::string(ui::text().status_read_only);
        if (mode_ == "unrestricted") return "unrestricted";
        return std::string(mode_ == "workspace" ? ui::text().status_auto_edit : ui::text().status_ask);
    }
    void update_prompt_footer() {
        header_->mode = planning_ ? "plan" : mode_; header_->model = model_label_; header_->invalidate();
        input_->set_footer(mode_label(), model_label_, busy_);
        input_->set_footer_tone(!planning_ && mode_ == "unrestricted", planning_);
        update_welcome();
    }

    void recall() {
        try {
            const nlohmann::json result = client_.call("input.recall_last", target());
            const auto& input = result["input"];
            if (input.is_object()) {
                const auto value = input.value("text", "");
                const auto submitted = std::find_if(submitted_.rbegin(), submitted_.rend(), [&](const auto& item) { return item.text == value; });
                if (submitted != submitted_.rend()) submitted_.erase(std::next(submitted).base());
                input_->set_text(value);
                prompt_changed();
            }
            refresh_snapshot();
        } catch (const std::exception& error) {
            toast(error.what(), tui::Notice::Severity::warn);
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
        hide_welcome();
        const auto submission = ++submission_seq_;
        submitted_.push_back({submission, text, local_time()});
        nlohmann::json params = target();
        params["text"] = std::move(text);
        request("input.submit", std::move(params), [this](const nlohmann::json&) {
            refresh_snapshot();
        }, [this, submission](const protocol::RpcError& error) {
            std::erase_if(submitted_, [&](const auto& item) { return item.id == submission; });
            toast(error.message, tui::Notice::Severity::warn);
        });
    }
    void command(const std::string& text) {
        const auto ui = std::ranges::find_if(command_ui_, [&](const CommandUi& item) { return item.alias == text; });
        if (ui == command_ui_.end()) toast(std::string(ui::text().toast_unknown_command) + text, tui::Notice::Severity::warn);
        else run_command(ui->id);
    }
    void compact() {
        if (busy_) return;
        request("session.compact", target(), [this](const nlohmann::json&) {
            set_busy(true); turn_meter_.set_phase(std::string(ui::text().act_compacting)); activity();
        }, command_failure());
    }
    void new_session() {
        if (busy_) return;
        request("session.new", target(), [this](const nlohmann::json& value) {
            apply_snapshot(value.get<protocol::SessionSnapshot>());
        }, command_failure());
        set_busy(true); turn_meter_.set_phase(std::string(ui::text().act_resuming)); activity();
    }
    void resume_session(std::string id) {
        if (busy_) return;
        nlohmann::json params = target();
        params["id"] = id;
        request("session.resume", std::move(params), [this](const nlohmann::json& value) {
            apply_snapshot(value.get<protocol::SessionSnapshot>());
        }, command_failure());
        set_busy(true); turn_meter_.set_phase(std::string(ui::text().act_resuming) + id.substr(0, 8)); activity();
    }

    void model_panel() {
        if (busy_) return;
        model_picker_.open(models_, model_name_);
        refresh_models();
    }
    void add_model() {
        if (busy_) return;
        model_dialog_.open(window_tokens_, provider_kinds_, [this](ModelInput model) {
            if (busy_) return;
            nlohmann::json params = target();
            params["kind"] = model.kind;
            params["name"] = model.name;
            params["base_url"] = model.base_url;
            params["model"] = model.model;
            params["credential"] = model.credential;
            params["max_tokens"] = model.max_tokens;
            params["context_window"] = model.context_window;
            request("model.add", std::move(params), [this](const nlohmann::json& value) {
                refresh_models();
                const bool selected = value.value("selected", false);
                std::string error;
                if (const auto it = value.find("selection_error"); it != value.end() && it->is_string())
                    error = it->get<std::string>();
                if (selected) toast(std::string(ui::text().toast_model_added) + value["model"].value("name", ""));
                else toast(error.empty() ? "model switch failed" : error, tui::Notice::Severity::warn);
            }, command_failure());
            set_busy(true); turn_meter_.set_phase(std::string(ui::text().act_adding_model)); activity();
        });
    }
    void switch_model(const std::string& name) {
        if (busy_ || name == model_name_) return;
        nlohmann::json params = target();
        params["name"] = name;
        request("session.select_model", std::move(params), [this](const nlohmann::json& value) {
            apply_snapshot(value.get<protocol::SessionSnapshot>());
        }, command_failure());
        set_busy(true); turn_meter_.set_phase(std::string(ui::text().act_switching_model)); activity();
    }

    void command_panel() {
        std::vector<Panel::Row> rows;
        for (const auto& command : keys_.commands()) {
            const auto meta = std::ranges::find_if(command_ui_, [&](const CommandUi& item) { return item.id == command.id; });
            const bool enabled = !command.enabled || command.enabled();
            rows.push_back({command.title, {}, meta == command_ui_.end() ? "" : meta->shortcut, enabled,
                [this, id = command.id] { run_command(id); }, command.category,
                meta == command_ui_.end() ? "" : meta->alias});
        }
        panel_.open(std::string(ui::text().panel_commands), std::move(rows),
                    format_text(ui::text().panel_command_footer, icons().up, icons().down),
                    true, {}, {}, 0, {}, std::string(ui::text().panel_commands_noun));
    }

    void skill_panel() {
        panel_.open(std::string(ui::text().panel_skills),
                    {{std::string(ui::text().panel_loading), {}, {}, false, {}}},
                    std::string(ui::text().panel_skill_footer), false);
        request("skills.list", {}, [this](const nlohmann::json& value) {
            std::vector<Panel::Row> rows;
            for (const auto& skill : value.value("skills", nlohmann::json::array())) {
                const std::string name = skill.value("name", "");
                rows.push_back({name, skill.value("description", ""), {}, true, [this, name] {
                    std::string text = input_->text();
                    if (!text.empty() && text.back() != ' ') text += ' ';
                    input_->set_text({});
                    input_->insert(text + "$" + name + " ");
                }});
            }
            if (rows.empty()) rows.push_back({std::string(ui::text().panel_no_skills), {}, {}, false, {}});
            for (const auto& item : value.value("diagnostics", nlohmann::json::array()))
                rows.push_back({item.value("path", ""), item.value("message", ""), "invalid", false, {}});
            panel_.open(std::string(ui::text().panel_skills), std::move(rows),
                        std::string(ui::text().panel_skill_footer));
        }, [this](const protocol::RpcError& error) {
            panel_.close();
            toast(std::string(ui::text().toast_skills_failed) + error.message, tui::Notice::Severity::error);
        });
    }
    void session_panel() {
        panel_.open(std::string(ui::text().panel_sessions), {{std::string(ui::text().panel_loading), {}, {}, false, {}}}, std::string(ui::text().panel_session_footer), false);
        request("session.list", {{"limit", 0}}, [this](const nlohmann::json& value) {
            std::vector<Panel::Row> rows;
            for (const auto& session : value.value("sessions", nlohmann::json::array())) {
                const std::string id = session.value("id", "");
                rows.push_back({session.value("title", "").empty() ? std::string(ui::text().panel_empty_session)
                                                                   : session.value("title", ""),
                                relative_age(std::chrono::system_clock::time_point(
                                    std::chrono::milliseconds(session.value("updated", std::int64_t{0})))),
                                id.substr(0, 4), true,
                                [this, id] { resume_session(id); }});
            }
            if (rows.empty()) rows.push_back({std::string(ui::text().panel_no_sessions), {}, {}, false, {}});
            panel_.open(std::string(ui::text().panel_sessions), std::move(rows), std::string(ui::text().panel_session_footer));
        }, [this](const protocol::RpcError& error) {
            panel_.close();
            toast(std::string(ui::text().toast_sessions_failed) + error.message, tui::Notice::Severity::error);
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
        request("session.grants", target(), [this](const nlohmann::json& value) {
            std::vector<Panel::Row> rows;
            for (const auto& grant : value.value("grants", nlohmann::json::array())) {
                const std::string id = grant.value("id", "");
                rows.push_back({grant.value("description", ""), {}, "revoke", true, [this, id] {
                    nlohmann::json params = target();
                    params["grant_id"] = id;
                    request("session.revoke_grant", std::move(params), [this](const nlohmann::json& result) {
                        const bool removed = result.value("removed", false);
                        toast(removed ? std::string(ui::text().toast_grant_revoked)
                                      : std::string(ui::text().toast_grant_absent),
                              removed ? tui::Notice::Severity::info : tui::Notice::Severity::warn);
                    }, [this](const protocol::RpcError& error) {
                        toast(error.message, tui::Notice::Severity::warn);
                    });
                }});
            }
            if (rows.empty()) rows.push_back({std::string(ui::text().panel_permissions_empty), {}, {}, false, {}});
            panel_.open("Session permissions", std::move(rows), std::string(ui::text().panel_permissions_footer), false);
        });
    }
    bool apply_appearance(const Appearance& value) {
        try {
            const auto selected = appearance_theme(value, themes_directory_, terminal_.caps().background);
            const bool icons_changed = value.icons != appearance_.icons;
            appearance_ = value;
            set_icons(value.icons);
            header_->outline = value.panels == "outline"; header_->invalidate_layout();
            hints_->outline = value.panels == "outline";
            side_->set_appearance(value);
            navigator_->set_appearance(value);
            update_panels(terminal_.size().cols);
            input_->set_appearance(value);
            queue_label_->outline = value.panels == "outline";
            frame_->welcome.appearance = value; frame_->welcome.invalidate();
            for (auto& pane : panes_) {
                pane.transcript->set_render_options(render_options());
                pane.transcript->set_compact(value.density == "compact");
                if (icons_changed) pane.transcript->refresh_icons();
            }
            update_todo_status(terminal_.size().cols);
            apply_theme(selected);
            return true;
        } catch (const std::exception& error) {
            toast(error.what(), tui::Notice::Severity::error); return false;
        }
    }
    void settings_panel(int row = 0) {
        completion_.close(); panel_.close();
        auto current = appearance_;
        if (std::filesystem::path(current.theme) == themes_directory_ / "dagent.json") current.theme = "dagent";
        const auto sidebar = sidebar_override_;
        const auto navigator = navigator_override_;
        const bool focused = navigator_focused_;
        settings_.open(current, layout_, [this](const Appearance& value, const Layout& layout) {
                if (!apply_appearance(value)) return false;
                apply_layout(layout); return true;
            },
            [this](const Appearance& value, const Layout& layout) {
                request("ui.appearance.save", {{"appearance", value.json()}, {"layout", layout.json()}},
                    [this](const nlohmann::json&) {
                        settings_.saved(); toasts_.show(ToastStack::Severity::success, std::string(ui::text().settings_saved));
                    }, [this](const protocol::RpcError& error) {
                        settings_.failed(); toast(error.message, tui::Notice::Severity::error);
                    });
            }, row, [this, sidebar, navigator, focused] {
                sidebar_override_ = sidebar; navigator_override_ = navigator;
                update_panels(terminal_.size().cols);
                if (focused && !navigator_->collapsed()) focus_navigator(); else focus_prompt();
            });
    }
    std::vector<KeyHint> key_hints() const {
        const auto hint = [](std::string_view key, std::string_view label) { return KeyHint{std::string(key), std::string(label)}; };
        if (dialog_.active()) {
            if (interaction_kind_ == "approval" && !permission_dialog_open_)
                return {hint("y", ui::text().hint_allow), hint("n", ui::text().hint_deny), hint("esc", ui::text().hint_deny)};
            return {hint("enter", ui::text().hint_confirm), hint("esc", ui::text().hint_cancel)};
        }
        if (model_picker_.active()) return model_picker_.hints();
        if (settings_.active()) return {hint("enter", ui::text().hint_save), hint("esc", ui::text().hint_revert)};
        if (panel_.visible() || completion_.visible() || model_dialog_.active())
            return {hint("enter", ui::text().hint_select), hint("esc", ui::text().hint_close)};
        if (navigator_focused_) return {hint("↑↓", ui::text().hint_select), hint("enter", ui::text().hint_open), hint("tab", ui::text().hint_section), hint("esc", ui::text().hint_back)};
        if (busy_) {
            std::vector<KeyHint> out{hint("esc", ui::text().hint_interrupt)};
            if (!snapshot_.queue.empty()) out.push_back(hint(icons().up, ui::text().hint_recall));
            if (panes_.size() > 1) out.push_back(hint("ctrl+a", ui::text().hint_agents));
            return out;
        }
        if (welcome_navigation()) return {hint(std::string(icons().up) + std::string(icons().down), ui::text().welcome_pick),
            hint("ctrl+p", ui::text().hint_commands), hint("ctrl+c", ui::text().welcome_quit)};
        std::vector<KeyHint> out{hint("ctrl+p", ui::text().hint_commands), hint("ctrl+t", ui::text().hint_sidebar), hint("shift+tab", ui::text().hint_mode)};
        if (terminal_.size().cols >= 110) out.push_back(hint("ctrl+b", ui::text().hint_navigator));
        if (!panes_.empty() && panes_[active_].transcript->can_switch_diff(*panes_[active_].scroll))
            out.push_back(hint("ctrl+d", unified_diff_ ? ui::text().hint_diff_split : ui::text().hint_diff_unified));
        return out;
    }

    void prompt_changed() { frame_->welcome.selected = -1; frame_->welcome.invalidate(); completion_.update(); }
    void interrupt() {
        if (!busy_) return;
        const nlohmann::json params = target();
        request("session.snapshot", params, [this, params](const nlohmann::json& snapshot) {
            const auto operation = snapshot.find("current_operation");
            if (operation == snapshot.end() || !operation->is_object()) return;
            const auto run = operation->find("run_id");
            if (run == operation->end() || !run->is_string()) return;
            const std::string run_id = run->get<std::string>();
            if (run_id.empty()) return;
            nlohmann::json cancel = params;
            cancel["run_id"] = run_id;
            request("run.cancel", std::move(cancel), {});
        });
    }
    void cancel() {
        if (completion_.visible()) { completion_.close(); return; }
        if (panel_.visible()) { panel_.close(); return; }
        if (model_picker_.active()) { model_picker_.close(); return; }
        if (busy_) { interrupt(); return; }
        if (active_ != 0) { show_pane(0); return; }
        if (!input_->text().empty()) { input_->set_text({}); prompt_changed(); return; }
        const auto now = Clock::now();
        if (last_cancel_ && now - *last_cancel_ < 1s) { exit(); return; }
        last_cancel_ = now; toast(std::string(ui::text().toast_exit));
    }
    void cycle_permission() {
        request("session.cycle_permission", target(), [this](const nlohmann::json& value) {
            apply_snapshot(value.get<protocol::SessionSnapshot>());
        }, [this](const protocol::RpcError& error) {
            toast(error.message, tui::Notice::Severity::warn);
        });
    }
    /// 快照里出现待确认的 unrestricted 升权：弹本地确认框，默认焦点在「取消」。
    void maybe_prompt_permission(const protocol::SessionSnapshot& snapshot) {
        if (!snapshot.permission_confirmation) {
            // 待确认状态消失（确认/取消/切换会话）：关闭本地确认框。
            if (permission_dialog_open_) { dialog_.close(); permission_dialog_open_ = false; }
            permission_prompt_ = false;
            return;
        }
        if (permission_prompt_) return;
        permission_prompt_ = true;
        permission_dialog_open_ = true;
        ui::QuestionRequest request;
        request.header = std::string(ui::text().permission_unrestricted_title);
        request.prompt = std::string(ui::text().permission_unrestricted_body);
        request.multi_select = false;
        request.allow_other = false;
        request.default_option = 1; // 取消
        request.allow_permission_cycle = true; // Shift+Tab 取消并跳到 ask
        request.options.push_back({std::string(ui::text().permission_enable), {}});
        request.options.push_back({std::string(ui::text().permission_cancel), {}});
        dialog_.open(std::move(request), [this](QuestionAnswer answer) {
            permission_dialog_open_ = false;
            const bool enable = !answer.cancelled && answer.selected.size() == 1 && answer.selected[0] == 0;
            set_permission_confirmation(enable);
        });
    }
    void set_permission_confirmation(bool confirmed) {
        nlohmann::json params = target();
        params["target"] = "unrestricted";
        params["confirmed"] = confirmed;
        request("session.set_permission", std::move(params), [this](const nlohmann::json& value) {
            apply_snapshot(value.get<protocol::SessionSnapshot>());
        }, [this](const protocol::RpcError& error) {
            toast(error.message, tui::Notice::Severity::warn);
        });
    }
    void toggle_plan() {
        if (busy_) return;
        request("session.toggle_planning", target(), [this](const nlohmann::json& value) {
            apply_snapshot(value.get<protocol::SessionSnapshot>());
        }, [this](const protocol::RpcError& error) {
            toast(error.message, tui::Notice::Severity::warn);
        });
    }
    void exit() {
        exiting_ = true; rt_.quit();
    }

    void show_interaction(const protocol::InteractionRequest& request) {
        interaction_kind_ = request.kind;
        active_interaction_ = request.interaction_id;
        // 后端交互对话框会替换本地 unrestricted 确认框；此时不再由快照关闭它。
        permission_dialog_open_ = false;
        if (request.kind == "approval") {
            dialog_.open(decode_approval(request.payload),
                         [this, id = request.interaction_id](ApprovalAnswer answer) {
                active_interaction_.clear();
                const char* decision = answer.decision == ApprovalAnswer::Decision::allow ? "allow"
                                     : answer.decision == ApprovalAnswer::Decision::allow_session ? "allow_session"
                                     : answer.decision == ApprovalAnswer::Decision::deny_with_feedback ? "deny_with_feedback"
                                                                                                       : "deny";
                answer_interaction(id, {{"decision", decision}, {"feedback", answer.feedback}});
            });
        } else {
            const auto question = decode_question(request.payload);
            if (request.session_id == id_) panes_.front().transcript->show_question(question);
            else for (auto& pane : panes_) if (pane.session_id == request.session_id)
                pane.transcript->show_question(question);
            dialog_.open(question,
                         [this, id = request.interaction_id](QuestionAnswer answer) {
                active_interaction_.clear();
                answer_interaction(id, {{"selected", answer.selected},
                                        {"other", answer.other},
                                        {"cancelled", answer.cancelled}});
            });
        }
    }
    void answer_interaction(std::string id, nlohmann::json answer) {
        auto gate = gate_;
        client_.call_async("interaction.answer", {{"interaction_id", id}, {"answer", std::move(answer)}},
                           [this, gate, id](std::expected<nlohmann::json, protocol::RpcError> result) {
            const std::lock_guard lock(gate->mutex);
            if (!gate->open || result) return;
            rt_.post([this, message = result.error().message] {
                toast(message, tui::Notice::Severity::warn);
            });
        });
    }

    client::Client& client_;
    FrontendBridge& bridge_;
    std::shared_ptr<CallbackGate> gate_ = std::make_shared<CallbackGate>();
    Appearance appearance_;
    Layout layout_;
    std::optional<bool> navigator_override_;
    bool navigator_focused_ = false;
    tui::TimerId git_debounce_ = 0;
    bool unified_diff_ = false;
    std::filesystem::path themes_directory_;
    std::optional<bool> sidebar_override_;
    Workspace* workspace_ = nullptr;
    bool resumed_ = false;
    std::vector<protocol::HistoryItem> history_;
    std::string initial_prompt_;
    std::vector<protocol::PublicModel> models_;
    std::vector<ProviderKind> provider_kinds_;
    protocol::SessionSnapshot snapshot_;
    std::shared_ptr<HistoryPager> history_pager_;
    tui::ThemeTokens theme_ = tui::dark_theme();
    uint32_t theme_epoch_ = 0;
    ScrollFrame* frame_ = nullptr;
    Activity* activity_ = nullptr;
    AgentTabs* tabs_ = nullptr;
    QueueLabel* queue_label_ = nullptr;
    PromptBox* input_ = nullptr;
    HeaderBar* header_ = nullptr;
    HintBar* hints_ = nullptr;
    SidePanel* side_ = nullptr;
    SidePanel* navigator_ = nullptr;
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
    MemoryBrowser memory_browser_;
    SettingsDialog settings_;
    ModelPicker model_picker_;
    std::string interaction_kind_;
    CompletionController completion_;
    std::string id_, error_, project_path_, branch_;
    std::filesystem::path project_root_;
    std::string active_interaction_, model_name_, model_label_;
    bool permission_prompt_ = false; ///< 本会话已就当前 pending 弹出过确认框
    bool permission_dialog_open_ = false; ///< 当前对话框仍是本地 unrestricted 确认框
    std::string mode_ = "ask";
    std::uint64_t generation_ = 0, state_seq_ = 0;
    bool planning_ = false, read_only_ = false;
    std::size_t window_tokens_ = 0;
    std::vector<McpStatus> mcp_states_;
    std::vector<CommandUi> command_ui_;
    TurnMeter turn_meter_;
    int animation_ = 0;
    struct Submitted { std::size_t id; std::string text, time; };
    std::vector<Submitted> submitted_;
    std::size_t submission_seq_ = 0;
    std::optional<Clock::time_point> last_cancel_;
    tui::TimerId activity_timer_ = 0, mcp_timer_ = 0;
    bool busy_ = false, exiting_ = false, signal_exit_ = false;
};

struct FrontendBridge::Impl {
    std::mutex mutex;
    Shell* shell = nullptr;
};

FrontendBridge::FrontendBridge() : impl_(std::make_unique<Impl>()) {}
FrontendBridge::~FrontendBridge() = default;

void FrontendBridge::attach(Shell& shell) {
    const std::lock_guard lock(impl_->mutex);
    impl_->shell = &shell;
}
void FrontendBridge::detach() {
    const std::lock_guard lock(impl_->mutex);
    impl_->shell = nullptr;
}
void FrontendBridge::event(const protocol::Event& event) {
    Shell* shell = nullptr;
    {
        const std::lock_guard lock(impl_->mutex);
        shell = impl_->shell;
    }
    if (shell) shell->on_backend_event(event);
}
void FrontendBridge::interaction_requested(const protocol::InteractionRequest& request) {
    Shell* shell = nullptr;
    {
        const std::lock_guard lock(impl_->mutex);
        shell = impl_->shell;
    }
    if (shell) shell->on_backend_interaction(request);
}
void FrontendBridge::interaction_closed(const std::string& interaction_id) {
    Shell* shell = nullptr;
    {
        const std::lock_guard lock(impl_->mutex);
        shell = impl_->shell;
    }
    if (shell) shell->on_backend_interaction_closed(interaction_id);
}
void FrontendBridge::disconnected(const std::string& message) {
    Shell* shell = nullptr;
    {
        const std::lock_guard lock(impl_->mutex);
        shell = impl_->shell;
    }
    if (shell) shell->on_backend_disconnected(message);
}

int run_interactive(client::Client& client, FrontendBridge& bridge, const InteractiveOptions& options,
                    std::stop_token stop) {

    std::filesystem::path project_root;
    std::string branch;
    if (!options.initial.session_id.empty()) {
        try {
            const nlohmann::json workspace =
                client.call("workspace.info", {{"session_id", options.initial.session_id}});
            project_root = workspace.value("project_root", "");
            branch = workspace.value("branch", "");
        } catch (const std::exception&) {
            // 项目信息只影响横幅与状态栏；缺失时保持空值。
        }
    }

    std::vector<protocol::HistoryItem> history;
    if (!options.initial.session_id.empty()) {
        try {
            history = load_history(client, options.initial.session_id);
        } catch (const std::exception&) {
            // 历史读取失败不阻止进入界面，历史区域保持空白。
        }
    }

    Shell shell(client, bridge, options.initial, std::move(history), options,
                std::move(project_root), std::move(branch));
    bridge.attach(shell);
    return shell.run(stop);
}

} // namespace dagent::ui
