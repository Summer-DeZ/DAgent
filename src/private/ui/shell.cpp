#include "ui/shell_detail.hpp"

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
#include <string>
#include <utility>
#include <vector>

#include "base/platform.hpp"
#include "base/path.hpp"
#include "base/overloaded.hpp"

namespace dagent::ui {
namespace {
using namespace std::chrono_literals;

std::string join_text(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) out += "; ";
        out += value;
    }
    return out;
}
std::string display_path(const std::filesystem::path& path) {
    const std::string value = base::path_to_utf8(path);
    if (const auto home = base::home_directory()) {
        const std::string& prefix = *home;
        if (value == prefix) return "~";
        if (value.starts_with(prefix + "/")) return "~" + value.substr(prefix.size());
    }
    return value;
}

} // namespace

std::string Shell::local_time() {
    const auto now = std::time(nullptr);
    std::tm local{}; base::local_time(now, local);
    char value[6]; std::strftime(value, sizeof(value), "%H:%M", &local);
    return value;
}

std::string Shell::relative_age(std::chrono::system_clock::time_point when) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now() - when)
                             .count();
    if (seconds < 60) return std::string(ui::text().panel_just_now);
    if (seconds < 3600) return format_text(ui::text().panel_minutes, seconds / 60);
    if (seconds < 86400) return format_text(ui::text().panel_hours, seconds / 3600);
    return format_text(ui::text().panel_days, seconds / 86400);
}

Shell::Shell(client::Client& client, FrontendBridge& bridge, const protocol::SessionSnapshot& initial,
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

Shell::~Shell() {
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

int Shell::run(std::stop_token stop) {
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

void Shell::on_backend_event(const protocol::Event& event) {
    rt_.post([this, event] { if (!exiting_) handle_event(event); });
}

void Shell::on_backend_interaction(const protocol::InteractionRequest& request) {
    rt_.post([this, request] { if (!exiting_) show_interaction(request); });
}

void Shell::on_backend_interaction_closed(const std::string& interaction_id) {
    rt_.post([this, interaction_id] {
        if (exiting_) return;
        if (active_interaction_ == interaction_id) {
            active_interaction_.clear();
            dialog_.close();
        }
    });
}

void Shell::on_backend_disconnected(const std::string& message) {
    rt_.post([this, message] {
        if (exiting_) return;
        error_ = message;
        exit();
    });
}

bool Shell::on_event(const tui::Event& event) {
    if (event.kind == tui::Event::Kind::focus && event.focus_gained) { schedule_git(); refresh_navigation_sessions(); }
    if (event.kind == tui::Event::Kind::resize) {
        update_panels(event.size.cols);
    }
    return keys_.on_event(event);
}

nlohmann::json Shell::target() const {
    return {{"session_id", id_}, {"session_generation", generation_}};
}

void Shell::request(std::string method, nlohmann::json params,
             std::function<void(const nlohmann::json&)> done,
             std::function<void(const protocol::RpcError&)> failed) {
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

std::function<void(const protocol::RpcError&)> Shell::command_failure() {
    return [this](const protocol::RpcError& error) {
        set_busy(false);
        if (error.kind == protocol::error_kind::kInvalidState ||
            error.kind == protocol::error_kind::kConfigError) {
            return;
        }
        toast(error.message, tui::Notice::Severity::warn);
    };
}

void Shell::refresh_snapshot() {
    request("session.snapshot", target(), [this](const nlohmann::json& value) {
        apply_snapshot(value.get<protocol::SessionSnapshot>());
    });
}

void Shell::update_context(std::size_t used, std::size_t limit) {
    header_->used = used; header_->limit = limit; header_->invalidate();
    side_->set_context(used, limit);
}

void Shell::apply_snapshot(const protocol::SessionSnapshot& snapshot) {
    if (snapshot.session_generation < generation_ || snapshot.state_seq < state_seq_) return;
    state_seq_ = snapshot.state_seq;
    snapshot_ = snapshot;
    if (!snapshot.sandbox_available && !sandbox_toast_shown_) {
        sandbox_toast_shown_ = true;
        toast(std::string(ui::text().sandbox_unavailable_toast), tui::Notice::Severity::warn);
    }
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

void Shell::handle_event(const protocol::Event& event) {
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

void Shell::apply_session_changed(const protocol::Event& event) {
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

void Shell::route_sub_event(const protocol::Event& event) {
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

void Shell::apply_live(const Event& event, bool update_state) {
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

void Shell::set_busy(bool value) {
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

void Shell::cancel_history() {
    if (history_pager_) history_pager_->cancel();
    history_pager_.reset();
}

void Shell::refresh_history() {
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

void Shell::refresh_project() {
    schedule_git();
    const std::string id = id_;
    request("workspace.info", {{"session_id", id}}, [this, id](const nlohmann::json& value) {
        if (id_ != id) return;
        branch_ = value.value("branch", "");
        header_->branch = branch_; header_->invalidate();
        update_welcome();
    });
}

void Shell::focus_prompt() {
    navigator_focused_ = false; navigator_->set_focused(false);
    rt_.set_focus(&prompt_, input_); hints_->invalidate();
}

void Shell::focus_navigator() {
    completion_.close(); navigator_focused_ = true; navigator_->set_focused(true);
    rt_.set_focus(navigator_, navigator_); hints_->invalidate();
}

void Shell::update_panels(int columns) {
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

void Shell::apply_layout(const Layout& value) {
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

void Shell::toggle_navigator() {
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

void Shell::refresh_navigation_sessions() {
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

void Shell::schedule_git() {
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

void Shell::refresh_navigation() {
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

void Shell::update_welcome() {
    auto& view = frame_->welcome;
    view.path = project_path_; view.branch = branch_; view.model = model_label_;
    view.mode = mode_label(); view.mcp = mcp_states_;
    if (mode_ == "unrestricted")
        view.notice = std::string(ui::text().permission_unrestricted_body);
    else if (!snapshot_.sandbox_available)
        view.notice = format_text(ui::text().sandbox_unavailable_notice,
                                  join_text(snapshot_.sandbox_missing));
    else
        view.notice.clear();
    view.invalidate();
}

void Shell::hide_welcome() {
    if (!frame_->welcome.visible) return;
    frame_->welcome.visible = false; frame_->welcome.selected = -1;
    input_->set_welcome(false);
    frame_->invalidate_tree();
}

bool Shell::welcome_navigation() const {
    return frame_->welcome.visible && frame_->welcome.has_recent() &&
           input_->text().empty() && snapshot_.queue.empty() && !busy_ && active_ == 0;
}

bool Shell::welcome_event(const tui::Event& e) {
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

void Shell::refresh_welcome() {
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

void Shell::refresh_models() {
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

bool Shell::mcp_pending() const {
    return std::ranges::any_of(mcp_states_, [](const McpStatus& state) {
        return state.status == "connecting" || state.status == "reconnecting";
    });
}

void Shell::refresh_mcp() {
    side_->set_mcp(mcp_states_);
    if (!mcp_pending() && !busy_) return;
    if (mcp_timer_) return;
    mcp_timer_ = rt_.every(200ms, [this] {
        refresh_snapshot();
        if (mcp_pending() || busy_) return true;
        mcp_timer_ = 0; return false;
    });
}

std::string Shell::mcp_label(const std::vector<McpStatus>& states) {
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

void Shell::toast(std::string text, tui::Notice::Severity severity) {
    toasts_.show(severity, std::move(text));
}

void Shell::apply_theme(const tui::ThemeTokens& selected) {
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

void Shell::activity() {
    if (!busy_) { activity_->set_action({}); return; }
    const auto meter = turn_meter_.display();
    activity_->set_action(format_text(ui::text().activity_elapsed,
        dialog_.active() ? std::string(interaction_kind_ == "question" && !permission_dialog_open_
            ? ui::text().act_waiting_answer : ui::text().act_waiting) : turn_meter_.phase(), meter.seconds, icons().separator),
        meter.tokens, meter.rate);
}

std::unique_ptr<tui::Widget> Shell::layout() {
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

bool Shell::apply_appearance(const Appearance& value) {
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
