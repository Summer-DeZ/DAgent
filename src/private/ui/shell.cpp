#include "ui/shell.hpp"

#include "ui/strings.hpp"
#include "ui/display.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
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

/// @brief 异步回调的寿命闸门：Shell 析构后回调直接丢弃，不再触碰页面或 Runtime。
struct CallbackGate {
    std::mutex mutex;
    bool open = true;
};

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

std::string relative_age(std::chrono::system_clock::time_point when) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now() - when)
                             .count();
    if (seconds < 60) return std::string(ui::text().panel_just_now);
    if (seconds < 3600) return format_text(ui::text().panel_minutes, seconds / 60);
    if (seconds < 86400) return format_text(ui::text().panel_hours, seconds / 3600);
    return format_text(ui::text().panel_days, seconds / 86400);
}

std::vector<int> subsequence_hits(std::string_view query, std::string_view value) {
    std::vector<int> hits;
    std::size_t from = 0;
    for (const char raw : query) {
        const char target = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
        std::size_t index = from;
        while (index < value.size() &&
               static_cast<char>(std::tolower(static_cast<unsigned char>(value[index]))) != target)
            ++index;
        if (index == value.size()) break;
        hits.push_back(static_cast<int>(index++));
        from = index;
    }
    return hits;
}

/// @brief 只读历史分页：每页结果经 rt.post 回到渲染线程；关闭/离开时释放未读完游标。
class HistoryPager : public std::enable_shared_from_this<HistoryPager> {
public:
    HistoryPager(client::Client& client, tui::Runtime& rt, std::shared_ptr<CallbackGate> gate,
                 std::string session_id)
        : client_(client), rt_(rt), gate_(std::move(gate)), session_id_(std::move(session_id)) {}

    std::function<void(const protocol::HistoryItem&)> on_item;
    std::function<void(std::string)> on_error;
    std::function<void()> on_done;

    void start() { request(); }
    void cancel() {
        std::string cursor;
        {
            const std::lock_guard lock(mutex_);
            cancelled_ = true;
            cursor = std::exchange(cursor_, {});
        }
        close_cursor(cursor);
    }

private:
    void close_cursor(const std::string& cursor) {
        if (!cursor.empty())
            client_.call_async("session.history_close", {{"cursor", cursor}},
                               [](std::expected<nlohmann::json, protocol::RpcError>) {});
    }

    void request() {
        nlohmann::json params{{"session_id", session_id_}, {"limit", 100}};
        {
            const std::lock_guard lock(mutex_);
            if (cancelled_) return;
            // 从这里到响应到达，游标归在途请求所有；cancel 由 handle 关闭响应中的新游标。
            if (!cursor_.empty()) params["cursor"] = std::exchange(cursor_, {});
        }
        auto self = shared_from_this();
        client_.call_async("session.history", std::move(params),
                           [self](std::expected<nlohmann::json, protocol::RpcError> response) {
                               self->handle(std::move(response));
                           });
    }

    void handle(std::expected<nlohmann::json, protocol::RpcError> response) {
        const std::lock_guard gate_lock(gate_->mutex);
        if (!response) {
            if (!gate_->open || cancelled()) return;
            auto self = shared_from_this();
            rt_.post([self, message = response.error().message] {
                const std::lock_guard lock(self->gate_->mutex);
                if (!self->gate_->open || self->cancelled()) return;
                if (self->on_error) self->on_error(message);
            });
            return;
        }
        std::vector<protocol::HistoryItem> items;
        for (const auto& item : response->value("items", nlohmann::json::array()))
            items.push_back(item.get<protocol::HistoryItem>());
        std::string next;
        if (const auto it = response->find("next_cursor"); it != response->end() && it->is_string())
            next = it->get<std::string>();
        bool discard;
        {
            const std::lock_guard lock(mutex_);
            discard = cancelled_ || !gate_->open;
            if (!discard) cursor_ = next;
        }
        if (discard) { close_cursor(next); return; }
        auto self = shared_from_this();
        rt_.post([self, items = std::move(items), done = next.empty()]() mutable {
            const std::lock_guard lock(self->gate_->mutex);
            if (!self->gate_->open || self->cancelled()) return;
            for (const auto& item : items) {
                if (self->on_item) self->on_item(item);
            }
            if (!done) self->request();
            else if (self->on_done) self->on_done();
        });
    }

    bool cancelled() {
        const std::lock_guard lock(mutex_);
        return cancelled_;
    }

    client::Client& client_;
    tui::Runtime& rt_;
    std::shared_ptr<CallbackGate> gate_;
    std::string session_id_;
    std::mutex mutex_;
    std::string cursor_;
    bool cancelled_ = false;
};

std::vector<protocol::HistoryItem> load_history(client::Client& client, const std::string& session_id) {
    std::vector<protocol::HistoryItem> items;
    std::string cursor;
    for (;;) {
        nlohmann::json params{{"session_id", session_id}, {"limit", 100}};
        if (!cursor.empty()) params["cursor"] = cursor;
        const nlohmann::json page = client.call("session.history", std::move(params));
        for (const auto& item : page.value("items", nlohmann::json::array())) {
            items.push_back(item.get<protocol::HistoryItem>());
        }
        if (const auto it = page.find("next_cursor"); it != page.end() && it->is_string()) {
            cursor = it->get<std::string>();
        } else {
            break;
        }
    }
    return items;
}

} // namespace

/// @brief 前端投影：只持 Client、只读 DTO 与页面状态，不持执行对象，不写业务状态。
class Shell final : public tui::EventHandler {
public:
    Shell(client::Client& client, FrontendBridge& bridge, const protocol::SessionSnapshot& initial,
          std::vector<protocol::HistoryItem> history, std::optional<ThemeSet> themes,
          const InteractiveOptions& options, std::filesystem::path project_root, std::string branch)
        : client_(client), bridge_(bridge), themes_(std::move(themes)),
          resumed_(options.resumed), history_(std::move(history)),
          initial_prompt_(options.initial_prompt), root_(layout()), rt_(terminal_, root_),
          prompt_(*input_, [this](std::string text) { submit(std::move(text)); },
                  [this] { recall(); }, [this] { prompt_changed(); },
                  [this] { return completion_.visible(); }),
          dialog_(rt_, [this] { interrupt(); }), model_dialog_(rt_),
          toasts_(rt_), panel_(rt_),
          completion_(rt_, *input_, [this] { completion_kind_.clear(); }),
          branch_(std::move(branch)), project_root_(std::move(project_root)) {
        apply_snapshot(initial);
        project_path_ = display_path(project_root_);
        status_->project(project_path_);
        side_->set_project(project_path_, branch_);
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
        reset_transcript(resumed_, history_.size());
        for (const auto& item : history_) active_transcript().append_history(item);
        history_.clear();
        refresh_project();
        refresh_mcp();
    }

    ~Shell() override {
        cancel_history();
        for (auto& pane : panes_) if (pane.pager) pane.pager->cancel();
        {
            const std::lock_guard lock(gate_->mutex);
            gate_->open = false;
        }
        rt_.cancel(file_debounce_);
        terminal_.restore();
        unbind_active_mouse();
    }

    int run(std::stop_token stop) {
        std::stop_callback signal(stop, [this] {
            rt_.post([this] { signal_exit_ = true; exit(); });
        });
        if (!initial_prompt_.empty()) submit(initial_prompt_);
        rt_.run();
        terminal_.restore();
        exiting_ = true;
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
        if (event.kind == tui::Event::Kind::resize) {
            active_transcript().set_todo_narrow(event.size.cols < 80);
            update_todo_status(event.size.cols);
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

    void apply_snapshot(const protocol::SessionSnapshot& snapshot) {
        if (snapshot.session_generation < generation_ || snapshot.state_seq < state_seq_) return;
        state_seq_ = snapshot.state_seq;
        snapshot_ = snapshot;
        id_ = snapshot.session_id;
        generation_ = snapshot.session_generation;
        model_name_ = snapshot.model.name;
        model_label_ = snapshot.model.model;
        planning_ = snapshot.planning;
        mode_ = snapshot.permission_mode;
        read_only_ = snapshot.read_only;
        worked_tokens_ = snapshot.context.used;
        token_limit_ = snapshot.context.limit;
        window_tokens_ = snapshot.context.window;
        mcp_states_ = decode_mcp(snapshot.mcp);
        side_->set_mcp(mcp_label(mcp_states_));
        status_->set_trigger(snapshot.context.trigger_percent);
        status_->context(worked_tokens_, token_limit_);
        side_->set_context(worked_tokens_, token_limit_);
        if (!panes_.empty()) {
            active_transcript().set_session(mode_label(), model_label_);
            update_prompt_footer();
        }
        set_busy(snapshot.busy);
        refresh_queue();
        refresh_mcp();
        const auto [done, total] = side_->progress();
        status_->todo(done, total, total > 0 && (side_->collapsed() || terminal_.size().cols < 80));
    }

    void handle_event(const protocol::Event& event) {
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
        if (event.parent_session_id) {
            route_sub_event(event);
            return;
        }
        if (event.session_id != id_) return;
        const bool update_state = event.seq > state_seq_;
        state_seq_ = std::max(state_seq_, event.seq);
        if (const auto decoded = decode_event(event)) apply_live(*decoded, update_state);
    }

    void apply_session_changed(const protocol::Event& event) {
        if (event.session_generation < generation_) return;
        cancel_history();
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
            active_transcript().set_session(mode_label(), model_label_);
            update_prompt_footer();
        }
        set_busy(false);
        refresh_project();
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
        phase_ = format_text(ui::text().act_task, agent);
        if (index == 0) return;
        const auto decoded = decode_event(event);
        if (!decoded) return;
        panes_[index].transcript->apply_live(*decoded);
    }

    void apply_live(const Event& event, bool update_state) {
        panes_.front().transcript->apply_live(event);
        std::visit(Overloaded{
                       [&](const TurnStarted&) { if (update_state) { set_busy(true); refresh_snapshot(); } },
                       [&](const StepStarted&) { phase_ = std::string(ui::text().act_thinking); step_begin_ = Clock::now(); },
                       [&](const TextDelta&) { phase_ = std::string(ui::text().act_generating); },
                       [&](const ReasoningDelta&) { phase_ = std::string(ui::text().act_generating); },
                       [&](const ToolPending& e) { phase_ = std::string(ui::text().act_preparing) + e.name; },
                       [&](const ToolStarted& e) { running_.push_back({e.id, e.summary, Clock::now()}); },
                       [&](const ToolFinished& e) {
                           std::erase_if(running_, [&](const Running& running) { return running.id == e.id; });
                       },
                       [&](const ContextUpdate& e) {
                           if (!update_state) return;
                           status_->context(e.used, e.limit);
                           side_->set_context(e.used, e.limit);
                       },
                       [&](const Retrying& e) {
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
                           active_transcript().set_session(mode_label(), model_label_);
                           update_prompt_footer();
                       },
                       [&](const TurnEnded&) {
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
            step_begin_ = Clock::now(); phase_ = std::string(ui::text().act_thinking);
            activity_timer_ = rt_.every(100ms, [this] { activity(); activity_->tick(); return true; });
        } else {
            running_.clear();
        }
        refresh_mcp(); activity();
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
        const std::string id = id_;
        request("workspace.info", {{"session_id", id}}, [this, id](const nlohmann::json& value) {
            if (id_ != id) return;
            branch_ = value.value("branch", "");
            side_->set_project(project_path_, branch_);
        });
    }

    void refresh_models() {
        request("model.list", nlohmann::json::object(), [this](const nlohmann::json& value) {
            models_.clear();
            for (const auto& model : value.value("models", nlohmann::json::array()))
                models_.push_back(model.get<protocol::PublicModel>());
            provider_kinds_.clear();
            for (const auto& kind : value.value("provider_kinds", nlohmann::json::array())) {
                const protocol::ProviderKind info = kind.get<protocol::ProviderKind>();
                provider_kinds_.push_back({info.kind, info.default_base_url, info.needs_credential});
            }
        });
    }

    void refresh_queue() {
        std::string label;
        const auto& queue = snapshot_.queue;
        if (!queue.empty()) {
            label = format_text(ui::text().queue_count, queue.size());
            const std::size_t first = queue.size() > 2 ? queue.size() - 2 : 0;
            for (std::size_t i = first; i < queue.size(); ++i)
                label += std::format("\n{} {}", i - first + 1, queue[i].text_preview);
        }
        queue_label_->set_text(std::move(label));
    }

    bool mcp_pending() const {
        return std::ranges::any_of(mcp_states_, [](const McpStatus& state) {
            return state.status == "connecting" || state.status == "reconnecting";
        });
    }
    void refresh_mcp() {
        side_->set_mcp(mcp_label(mcp_states_));
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
        return format_text(ui::text().status_mcp, ready, states.size());
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
        add_command("skill.pick", std::string(ui::text().cmd_skills), std::string(ui::text().cmd_session), {}, "/skills", [this] { skill_panel(); });
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
        std::shared_ptr<HistoryPager> pager;
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
            scroll.document(), [this, index](const TodoList& value) {
                if (index == 0) update_todo(value);
            });
        transcript->set_session(mode_label(), model_label_);
        scroll.set_theme(theme_);
        auto mouse = std::make_unique<tui::ScrollbackMouse>(rt_, scroll);
        panes_.push_back({&scroll, std::move(transcript), std::move(mouse), {}, std::move(title),
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
            if (panes_[i].pager) panes_[i].pager->cancel();
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

    void update_todo(const TodoList& value) {
        side_->set_items(value);
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
        if (mode_ == "unrestricted") return "unrestricted";
        return std::string(mode_ == "workspace" ? ui::text().status_auto_edit : ui::text().status_ask);
    }
    void update_prompt_footer() {
        input_->set_footer(format_text(ui::text().box_footer, mode_label(), model_label_));
        input_->set_footer_tone(!planning_ && mode_ == "unrestricted", planning_);
    }

    void recall() {
        try {
            const nlohmann::json result = client_.call("input.recall_last", target());
            const auto& input = result["input"];
            if (input.is_object()) {
                input_->set_text(input.value("text", ""));
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
        set_session_title(text);
        nlohmann::json params = target();
        params["text"] = std::move(text);
        request("input.submit", std::move(params), [this](const nlohmann::json&) {
            refresh_snapshot();
        }, [this](const protocol::RpcError& error) {
            toast(error.message, tui::Notice::Severity::warn);
        });
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
        request("session.compact", target(), [this](const nlohmann::json&) {
            set_busy(true); phase_ = std::string(ui::text().act_compacting); activity();
        }, command_failure());
    }
    void new_session() {
        if (busy_) return;
        request("session.new", target(), [this](const nlohmann::json& value) {
            apply_snapshot(value.get<protocol::SessionSnapshot>());
        }, command_failure());
        set_busy(true); phase_ = std::string(ui::text().act_resuming); activity();
    }
    void resume_session(std::string id) {
        if (busy_) return;
        nlohmann::json params = target();
        params["id"] = id;
        request("session.resume", std::move(params), [this](const nlohmann::json& value) {
            apply_snapshot(value.get<protocol::SessionSnapshot>());
        }, command_failure());
        set_busy(true); phase_ = std::string(ui::text().act_resuming) + id.substr(0, 8); activity();
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
            set_busy(true); phase_ = std::string(ui::text().act_adding_model); activity();
        });
    }
    void switch_model(const std::string& name) {
        if (busy_ || name == model_name_) return;
        nlohmann::json params = target();
        params["name"] = name;
        request("session.select_model", std::move(params), [this](const nlohmann::json& value) {
            apply_snapshot(value.get<protocol::SessionSnapshot>());
        }, command_failure());
        set_busy(true); phase_ = std::string(ui::text().act_switching_model); activity();
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
        request("session.list", {{"limit", 50}}, [this](const nlohmann::json& value) {
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
                        toast(removed ? "Session permission revoked" : "Permission was already absent",
                              removed ? tui::Notice::Severity::info : tui::Notice::Severity::warn);
                    }, [this](const protocol::RpcError& error) {
                        toast(error.message, tui::Notice::Severity::warn);
                    });
                }});
            }
            if (rows.empty()) rows.push_back({"No active session permissions", {}, {}, false, {}});
            panel_.open("Session permissions", std::move(rows), "enter revoke · esc close", false);
        });
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
        if (begin < value.size() && value[begin] == '$') {
            if (completion_kind_ != "skill") {
                completion_kind_ = "skill";
                completion_.open(std::string(ui::text().panel_skills), [this](std::string_view query, auto done) {
                    request("skills.list", {}, [query = std::string(query), done = std::move(done)](const nlohmann::json& value) mutable {
                        std::vector<Completion::Item> items;
                        for (const auto& skill : value.value("skills", nlohmann::json::array())) {
                            const std::string name = skill.value("name", "");
                            const auto hits = subsequence_hits(query, name);
                            if (query.empty() || hits.size() == query.size())
                                items.push_back({"$" + name, name, skill.value("description", ""), hits});
                        }
                        done(std::move(items));
                    });
                }, std::string(ui::text().panel_skill_footer),
                [this](const Completion::Item& item, bool) {
                    std::string text = input_->text();
                    const std::size_t split = text.find_last_of(" \t\r\n");
                    text.erase(split == std::string::npos ? 0 : split + 1);
                    text += item.value + " "; input_->set_text({}); input_->insert(text);
                });
            }
            completion_.refresh(std::string_view(value).substr(begin + 1)); return;
        }
        if (begin < value.size() && value[begin] == '@') {
            if (completion_kind_ != "file") {
                completion_kind_ = "file";
                completion_.open(std::string(ui::text().panel_files), [this](std::string_view query, auto done) {
                    rt_.cancel(file_debounce_);
                    file_debounce_ = rt_.after(80ms, [this, query = std::string(query), done = std::move(done)]() mutable {
                        nlohmann::json params{{"session_id", id_}, {"text", query}, {"limit", 8}};
                        request("workspace.complete", std::move(params), [done = std::move(done), query](const nlohmann::json& value) mutable {
                            std::vector<Completion::Item> items;
                            for (const auto& candidate : value.value("candidates", nlohmann::json::array())) {
                                const std::string path = candidate.value("path", "");
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
        active_interaction_ = request.interaction_id;
        if (request.kind == "approval") {
            dialog_.open(decode_approval(request.payload),
                         [this, id = request.interaction_id](ApprovalAnswer answer) {
                active_interaction_.clear();
                const char* decision = answer.decision == ApprovalAnswer::Decision::allow ? "allow"
                                     : answer.decision == ApprovalAnswer::Decision::allow_session ? "allow_session"
                                     : answer.decision == ApprovalAnswer::Decision::deny_with_feedback ? "deny_with_feedback"
                                                                                                       : "deny";
                answer_interaction(id, {{"decision", decision},
                                        {"feedback", answer.feedback},
                                        {"network", answer.network}});
            });
        } else {
            dialog_.open(decode_question(request.payload),
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
    std::optional<ThemeSet> themes_;
    bool resumed_ = false;
    std::vector<protocol::HistoryItem> history_;
    std::string initial_prompt_;
    std::vector<protocol::PublicModel> models_;
    std::vector<ProviderKind> provider_kinds_;
    protocol::SessionSnapshot snapshot_;
    std::shared_ptr<HistoryPager> history_pager_;
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
    std::string mode_ = "ask";
    std::uint64_t generation_ = 0, state_seq_ = 0;
    bool planning_ = false, read_only_ = false;
    std::size_t worked_tokens_ = 0, token_limit_ = 0, window_tokens_ = 0;
    std::vector<McpStatus> mcp_states_;
    std::vector<CommandUi> command_ui_;
    struct Running { std::string id, summary; Clock::time_point begin; };
    std::vector<Running> running_;
    Clock::time_point step_begin_{};
    std::optional<Clock::time_point> last_cancel_;
    tui::TimerId activity_timer_ = 0, mcp_timer_ = 0, file_debounce_ = 0;
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
    std::optional<ThemeSet> themes;
    if (!options.theme_file.empty()) themes = load_theme(options.theme_file);

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

    Shell shell(client, bridge, options.initial, std::move(history), std::move(themes), options,
                std::move(project_root), std::move(branch));
    bridge.attach(shell);
    return shell.run(stop);
}

} // namespace dagent::ui
