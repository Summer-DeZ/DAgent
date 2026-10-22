/// @file shell_detail.hpp
/// @brief 交互界面的 Shell 类声明；实现按职责分在 shell.cpp（生命周期、事件、快照与布局）、
/// shell_commands.cpp（命令、面板、权限动作与交互回答）和 shell_panes.cpp（子会话 Pane）。
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "ui/shell.hpp"
#include "tui/document/document.hpp"
#include "ui/overlay/approval.hpp"
#include "ui/overlay/completion_controller.hpp"
#include "ui/overlay/memory_browser.hpp"
#include "ui/overlay/model_dialog.hpp"
#include "ui/overlay/model_picker.hpp"
#include "ui/overlay/panel.hpp"
#include "ui/overlay/settings_dialog.hpp"
#include "ui/overlay/toast.hpp"
#include "ui/page/center.hpp"
#include "ui/page/header_bar.hpp"
#include "ui/page/hint_bar.hpp"
#include "ui/page/prompt_box.hpp"
#include "ui/page/prompt_input.hpp"
#include "ui/page/side_panel.hpp"
#include "ui/page/transcript.hpp"
#include "ui/page/welcome.hpp"
#include "ui/state/callback_gate.hpp"
#include "ui/state/history_pager.hpp"
#include "ui/state/turn_meter.hpp"
#include "ui/style/strings.hpp"
#include "ui/style/theme_config.hpp"
#include "ui/page/workspace.hpp"

namespace dagent::ui {

class Shell final : public tui::EventHandler {
    using Clock = std::chrono::steady_clock;
public:
    Shell(client::Client& client, FrontendBridge& bridge, const protocol::SessionSnapshot& initial,
          std::vector<protocol::HistoryItem> history,
          const InteractiveOptions& options, std::filesystem::path project_root, std::string branch);
    ~Shell() override;
    int run(std::stop_token stop);

    // ---- FrontendBridge：后端线程 → 渲染线程（Shell 已 detach 后不再到达）----
    void on_backend_event(const protocol::Event& event);
    void on_backend_interaction(const protocol::InteractionRequest& request);
    void on_backend_interaction_closed(const std::string& interaction_id);
    void on_backend_disconnected(const std::string& message);
    bool on_event(const tui::Event& event) override;

private:
    static std::string local_time();
    static std::string relative_age(std::chrono::system_clock::time_point when);
    struct CommandUi { std::string id, shortcut, alias; };

    // ---- RPC：请求在读取线程返回，回调经 rt.post 回到渲染线程 ----
    nlohmann::json target() const;
    void request(std::string method, nlohmann::json params,
                 std::function<void(const nlohmann::json&)> done,
                 std::function<void(const protocol::RpcError&)> failed = {});
    /// @brief 命令的失败处理：解除本地 pending；执行期失败已有 notice 事件，不重复提示。
    std::function<void(const protocol::RpcError&)> command_failure();
    void refresh_snapshot();
    void update_context(std::size_t used, std::size_t limit);
    void apply_snapshot(const protocol::SessionSnapshot& snapshot);
    void handle_event(const protocol::Event& event);
    void apply_session_changed(const protocol::Event& event);
    void route_sub_event(const protocol::Event& event);
    void apply_live(const Event& event, bool update_state);
    void set_busy(bool value);
    void cancel_history();
    void refresh_history();
    void refresh_project();
    void focus_prompt();
    void focus_navigator();
    void update_panels(int columns);
    void apply_layout(const Layout& value);
    void toggle_navigator();
    void refresh_navigation_sessions();
    void schedule_git();
    void refresh_navigation();
    void update_welcome();
    void hide_welcome();
    bool welcome_navigation() const;
    bool welcome_event(const tui::Event& e);
    void refresh_welcome();
    void refresh_models();

    void refresh_queue() { queue_label_->set_queue(snapshot_.queue); }

    bool mcp_pending() const;
    void refresh_mcp();
    static std::string mcp_label(const std::vector<McpStatus>& states);
    void toast(std::string text, tui::Notice::Severity severity = tui::Notice::Severity::info);
    void apply_theme(const tui::ThemeTokens& selected);
    void activity();
    std::unique_ptr<tui::Widget> layout();

    void add_command(std::string id, std::string title, std::string category,
                     std::string shortcut, std::string alias, std::function<void()> fn,
                     std::function<bool()> enabled = {});
    void register_commands();
    void run_command(std::string_view id);
    void reset_transcript(bool resumed = false, std::size_t messages = 0);

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
    void refresh_agents();
    tui::Scrollback& active_scroll() { return *panes_[active_].scroll; }
    Transcript& active_transcript() { return *panes_[active_].transcript; }
    void bind_active_mouse() { rt_.bind_mouse(active_scroll(), *panes_[active_].mouse); }
    void unbind_active_mouse() { rt_.unbind_mouse(active_scroll()); }

    RenderOptions render_options() const;
    std::size_t add_pane(std::string title, std::string session_id, std::string call_id);
    void show_pane(std::size_t index);
    void close_child_panes();
    /// 恢复出来的 task 没有 Pane：切进去时再回放子会话记录。
    void open_task_pane(const Transcript::TaskRef& task);
    void agent_panel();
    void update_todo(const TodoList& value);
    void update_todo_status(int columns);
    void toggle_todo();
    /// 输入框尾行与消息尾行共用同一份「模式 · 模型」。
    std::string mode_label() const;
    void update_prompt_footer();
    void recall();
    void submit(std::string text);
    void command(const std::string& text);
    void compact();
    void new_session();
    void resume_session(std::string id);
    void model_panel();
    void add_model();
    void switch_model(const std::string& name);
    void command_panel();
    void skill_panel();
    void session_panel();
    void help_panel();
    void permissions_panel();
    bool apply_appearance(const Appearance& value);
    void settings_panel(int row = 0);
    std::vector<KeyHint> key_hints() const;

    void prompt_changed() { frame_->welcome.selected = -1; frame_->welcome.invalidate(); completion_.update(); }
    void interrupt();
    void cancel();
    void cycle_permission();
    /// 快照里出现待确认的 unrestricted 升权：弹本地确认框，默认焦点在「取消」。
    void maybe_prompt_permission(const protocol::SessionSnapshot& snapshot);
    void set_permission_confirmation(bool confirmed);
    void toggle_plan();
    void exit();
    void show_interaction(const protocol::InteractionRequest& request);
    void answer_interaction(std::string id, nlohmann::json answer);
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
    bool sandbox_toast_shown_ = false; ///< 已提示过沙箱不可用
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

} // namespace dagent::ui
