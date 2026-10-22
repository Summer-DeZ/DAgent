#include "ui/shell_detail.hpp"

#include <algorithm>
#include <expected>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/platform.hpp"

namespace dagent::ui {

namespace {
using namespace std::chrono_literals;
} // namespace

void Shell::add_command(std::string id, std::string title, std::string category,
                 std::string shortcut, std::string alias, std::function<void()> fn,
                 std::function<bool()> enabled) {
    keys_.add({id, std::move(title), std::move(category), std::move(fn), std::move(enabled)});
    command_ui_.push_back({id, shortcut, alias});
    if (!shortcut.empty() && !keys_.bind(shortcut, id))
        throw std::logic_error("invalid key binding: " + shortcut);
}

void Shell::register_commands() {
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

void Shell::run_command(std::string_view id) {
    const auto it = std::ranges::find_if(keys_.commands(), [&](const tui::Command& command) {
        return command.id == id;
    });
    if (it != keys_.commands().end() && (!it->enabled || it->enabled())) it->run();
}

void Shell::update_todo(const TodoList& value) {
    side_->set_items(value);
    update_todo_status(terminal_.size().cols);
}

void Shell::update_todo_status(int columns) {
    const bool hidden = side_->collapsed() || columns < 80;
    header_->sidebar = !hidden; header_->invalidate();
}

void Shell::toggle_todo() {
    sidebar_override_ = !side_->collapsed();
    update_panels(terminal_.size().cols);
}

std::string Shell::mode_label() const {
    if (planning_) return "plan";
    if (read_only_) return std::string(ui::text().status_read_only);
    if (mode_ == "unrestricted") return "unrestricted";
    return std::string(mode_ == "workspace" ? ui::text().status_auto_edit : ui::text().status_ask);
}

void Shell::update_prompt_footer() {
    header_->mode = planning_ ? "plan" : mode_; header_->model = model_label_; header_->invalidate();
    input_->set_footer(mode_label(), model_label_, busy_);
    input_->set_footer_tone(!planning_ && mode_ == "unrestricted", planning_);
    update_welcome();
}

void Shell::recall() {
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

void Shell::submit(std::string text) {
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

void Shell::command(const std::string& text) {
    const auto ui = std::ranges::find_if(command_ui_, [&](const CommandUi& item) { return item.alias == text; });
    if (ui == command_ui_.end()) toast(std::string(ui::text().toast_unknown_command) + text, tui::Notice::Severity::warn);
    else run_command(ui->id);
}

void Shell::compact() {
    if (busy_) return;
    request("session.compact", target(), [this](const nlohmann::json&) {
        set_busy(true); turn_meter_.set_phase(std::string(ui::text().act_compacting)); activity();
    }, command_failure());
}

void Shell::new_session() {
    if (busy_) return;
    request("session.new", target(), [this](const nlohmann::json& value) {
        apply_snapshot(value.get<protocol::SessionSnapshot>());
    }, command_failure());
    set_busy(true); turn_meter_.set_phase(std::string(ui::text().act_resuming)); activity();
}

void Shell::resume_session(std::string id) {
    if (busy_) return;
    nlohmann::json params = target();
    params["id"] = id;
    request("session.resume", std::move(params), [this](const nlohmann::json& value) {
        apply_snapshot(value.get<protocol::SessionSnapshot>());
    }, command_failure());
    set_busy(true); turn_meter_.set_phase(std::string(ui::text().act_resuming) + id.substr(0, 8)); activity();
}

void Shell::model_panel() {
    if (busy_) return;
    model_picker_.open(models_, model_name_);
    refresh_models();
}

void Shell::add_model() {
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

void Shell::switch_model(const std::string& name) {
    if (busy_ || name == model_name_) return;
    nlohmann::json params = target();
    params["name"] = name;
    request("session.select_model", std::move(params), [this](const nlohmann::json& value) {
        apply_snapshot(value.get<protocol::SessionSnapshot>());
    }, command_failure());
    set_busy(true); turn_meter_.set_phase(std::string(ui::text().act_switching_model)); activity();
}

void Shell::command_panel() {
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

void Shell::skill_panel() {
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

void Shell::session_panel() {
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

void Shell::help_panel() {
    std::vector<Panel::Row> rows;
    for (const auto& command : keys_.commands()) {
        const auto meta = std::ranges::find_if(command_ui_, [&](const CommandUi& item) { return item.id == command.id; });
        std::string key;
        if (meta != command_ui_.end()) key = !meta->shortcut.empty() ? meta->shortcut : meta->alias;
        rows.push_back({key, command.title, command.category, false, {}});
    }
    panel_.open(std::string(ui::text().panel_help), std::move(rows), std::string(ui::text().panel_close), false);
}

void Shell::permissions_panel() {
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

void Shell::settings_panel(int row) {
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

std::vector<KeyHint> Shell::key_hints() const {
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

void Shell::interrupt() {
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

void Shell::cancel() {
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

void Shell::cycle_permission() {
    request("session.cycle_permission", target(), [this](const nlohmann::json& value) {
        apply_snapshot(value.get<protocol::SessionSnapshot>());
    }, [this](const protocol::RpcError& error) {
        toast(error.message, tui::Notice::Severity::warn);
    });
}

void Shell::maybe_prompt_permission(const protocol::SessionSnapshot& snapshot) {
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

void Shell::set_permission_confirmation(bool confirmed) {
    nlohmann::json params = target();
    params["target"] = "unrestricted";
    params["confirmed"] = confirmed;
    request("session.set_permission", std::move(params), [this](const nlohmann::json& value) {
        apply_snapshot(value.get<protocol::SessionSnapshot>());
    }, [this](const protocol::RpcError& error) {
        toast(error.message, tui::Notice::Severity::warn);
    });
}

void Shell::toggle_plan() {
    if (busy_) return;
    request("session.toggle_planning", target(), [this](const nlohmann::json& value) {
        apply_snapshot(value.get<protocol::SessionSnapshot>());
    }, [this](const protocol::RpcError& error) {
        toast(error.message, tui::Notice::Severity::warn);
    });
}

void Shell::exit() {
    exiting_ = true; rt_.quit();
}

void Shell::show_interaction(const protocol::InteractionRequest& request) {
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

void Shell::answer_interaction(std::string id, nlohmann::json answer) {
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

} // namespace dagent::ui
