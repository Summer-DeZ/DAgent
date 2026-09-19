#include "ui/shell.hpp"

#include <condition_variable>
#include <deque>
#include <format>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

#include "agent/agent.hpp"
#include "base/log.hpp"
#include "ui/approval.hpp"
#include "ui/prompt_input.hpp"
#include "ui/status_line.hpp"
#include "ui/theme_config.hpp"
#include "ui/transcript.hpp"

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

// 内部 Shell 把线程归属和控件寿命封装起来，对外只暴露 run_interactive。
class Shell {
public:
    Shell(agent::Setup setup, std::unique_ptr<agent::Agent> agent,
          std::vector<agent::Event> history, std::optional<ThemeSet> themes)
        : setup_(std::move(setup)), agent_(std::move(agent)), themes_(std::move(themes)),
          root_(layout()), rt_(terminal_, root_), transcript_(scroll_->document()),
          prompt_(*input_, [this](std::string text) { submit(std::move(text)); }, [this] { recall(); }),
          keys_(rt_), mouse_(rt_, *scroll_), dialog_(rt_, [this] { interrupt(); }) {
        id_ = agent_->meta().id;
        status_->session(setup_.model.model, id_);
        status_->set_trigger(setup_.options.context.compaction_trigger_percent);
        rt_.set_focus(&prompt_, input_); rt_.set_global(keys_);
        rt_.bind_mouse(*scroll_, mouse_);
        rt_.on_caps([this](const tui::Terminal::Caps& caps) {
            theme_ = themes_ ? themes_->pick(caps.background) : tui::default_theme(caps.background);
            ++theme_.epoch;
            scroll_->set_theme(theme_); activity_->set_theme(theme_); notice_->set_theme(theme_);
            queue_label_->set_theme(theme_); queue_label_->set_style(theme_.accent);
            input_->set_theme(theme_); status_->set_theme(theme_); dialog_.set_theme(theme_);
        });
        bind("session.interrupt", "中断", "escape", [this] { interrupt(); });
        bind("session.cancel", "清空或退出", "ctrl+c", [this] { cancel(); });
        bind("permission.cycle", "切换权限", "shift+tab", [this] { cycle_permission(); });
        bind("tools.expand", "展开工具输出", "ctrl+o", [this] { transcript_.toggle_tools(); });
        bind("scroll.up", "向上翻页", "pageup", [this] { scroll_->scroll_pages(-1); });
        bind("scroll.down", "向下翻页", "pagedown", [this] { scroll_->scroll_pages(1); });
        reset_transcript();
        for (const auto& event : history) apply(event);
        worker_ = std::jthread([this](std::stop_token stop) {
            try {
                while (auto job = jobs_.pop(stop)) (*job)();
            } catch (const std::exception& e) { worker_failed(e.what()); }
            catch (...) { worker_failed("agent 线程发生未知异常"); }
        });
    }

    ~Shell() {
        turn_stop_.request_stop(); jobs_.close(); worker_.request_stop();
        terminal_.restore();
        if (worker_.joinable()) worker_.join();
        rt_.unbind_mouse(*scroll_);
    }

    int run(const std::string& initial, agent::Interrupts& interrupts) {
        // 整个全屏期间都走优雅退出，包含空闲和等待审批；第二个进程信号仍可强制退出。
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
        if (!error_.empty()) std::cerr << "运行失败：" << error_ << '\n';
        std::cout << "会话 " << id_ << " 已保存，用 dagent -r " << id_ << " 继续\n";
        return !error_.empty() ? 1 : signal_exit_ ? 130 : 0;
    }

private:
    std::unique_ptr<tui::Widget> layout() {
        auto body = std::make_unique<tui::Container>();
        auto add = [&]<class T>(T*& target, tui::Constraint constraint) {
            auto widget = std::make_unique<T>(); target = widget.get(); body->add(constraint, std::move(widget));
        };
        add(scroll_, {tui::Sizing::flex, 1});
        add(activity_, {tui::Sizing::content}); add(notice_, {tui::Sizing::content});
        add(queue_label_, {tui::Sizing::content, 0, 0, 3});
        add(input_, {tui::Sizing::content, 0, 3, 8}); add(status_, {tui::Sizing::fixed, 1});
        return body;
    }
    void bind(std::string id, std::string title, std::string_view key, std::function<void()> fn) {
        keys_.add({id, std::move(title), "会话", std::move(fn), {}});
        if (!keys_.bind(key, id)) throw std::logic_error("无效快捷键：" + std::string(key));
    }
    void reset_transcript() {
        transcript_.clear();
        transcript_.info("DAgent · /help 查看按键和命令");
    }
    void worker_failed(std::string error) {
        base::logger("ui")->error("agent 线程退出：{}", error);
        rt_.post([this, error = std::move(error)] {
            error_ = error; transcript_.apply(agent::Notice{agent::Notice::Level::error, error}); exit();
        });
    }
    void notice(std::string text, tui::Notice::Severity severity = tui::Notice::Severity::info) {
        notice_->show(severity, std::move(text)); rt_.cancel(notice_timer_);
        notice_timer_ = rt_.after(5s, [this] { notice_->show(tui::Notice::Severity::info, {}); });
    }
    void activity() {
        if (!busy_) { activity_->set_action({}); return; }
        if (dialog_.active()) { activity_->set_action("等待你的确认"); return; }
        std::string label = phase_;
        auto begin = step_begin_;
        if (!running_.empty()) { label = "运行 " + running_.front().summary; begin = running_.front().begin; }
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - begin).count();
        activity_->set_action(std::format("{} {}s", label, seconds));
    }
    void busy(bool value) {
        busy_ = value; rt_.cancel(activity_timer_); activity_timer_ = 0;
        if (value) {
            step_begin_ = Clock::now(); phase_ = "思考中";
            activity_timer_ = rt_.every(100ms, [this] { activity(); activity_->tick(); return true; });
        } else running_.clear();
        activity();
    }
    void apply(const agent::Event& event) {
        transcript_.apply(event);
        std::visit(Overloaded{
            [&](const agent::StepStarted&) { phase_ = "思考中"; step_begin_ = Clock::now(); },
            [&](const agent::TextDelta&) { phase_ = "生成中"; },
            [&](const agent::ReasoningDelta&) { phase_ = "生成中"; },
            [&](const agent::ToolPending& e) { phase_ = "准备调用 " + e.name; },
            [&](const agent::ToolStarted& e) { running_.push_back({e.id, e.summary, Clock::now()}); },
            [&](const agent::ToolFinished& e) {
                std::erase_if(running_, [&](const Running& r) { return r.id == e.id; });
            },
            [&](const agent::ContextUpdate& e) { status_->context(e); },
            [&](const agent::Retrying& e) {
                notice(std::format("{}，{:.1f} 秒后重试（{}/{}）", e.reason, e.wait.count() / 1000.0,
                                   e.attempt, e.max_attempts), tui::Notice::Severity::warn);
            },
            [&](const agent::Notice& e) {
                notice(e.text, e.level == agent::Notice::Level::error ? tui::Notice::Severity::error
                               : e.level == agent::Notice::Level::warn ? tui::Notice::Severity::warn
                                                                     : tui::Notice::Severity::info);
            },
            [&](const agent::TurnEnded&) { dialog_.close(); busy(false); drain(); },
            [](const auto&) {}
        }, event);
        activity();
    }
    void refresh_queue() {
        std::string label;
        for (const auto& value : pending_) {
            if (!label.empty()) label += '\n';
            label += "排队：" + value.substr(0, value.find('\n'));
        }
        queue_label_->set_text(std::move(label));
    }
    void recall() {
        if (pending_.empty()) return;
        input_->set_text(std::move(pending_.back())); pending_.pop_back(); refresh_queue();
    }
    void submit(std::string text) {
        if (exiting_) return;
        pending_.push_back(std::move(text)); refresh_queue(); drain();
    }
    void drain() {
        while (!busy_ && !exiting_ && !pending_.empty()) {
            auto text = std::move(pending_.front()); pending_.pop_front(); refresh_queue();
            if (text.starts_with('/')) { command(text); continue; }
            busy(true); turn_stop_ = std::stop_source{};
            auto token = turn_stop_.get_token();
            jobs_.push([this, text = std::move(text), token] {
                const agent::Sink sink = [this](const agent::Event& event) {
                    rt_.post([this, event] { if (!exiting_) apply(event); });
                };
                agent_->run_turn(text, sink, [this](const agent::Approval& a, std::stop_token stop) {
                    return approve(rt_, dialog_, a, stop);
                }, token);
            });
        }
    }
    void command(const std::string& text) {
        if (text == "/exit") exit();
        else if (text == "/help") transcript_.info(
            "Enter 发送；忙时排队；空输入 ↑ 取回排队消息\n"
            "Shift/Alt+Enter 换行；Esc 中断；Ctrl+C 清空/中断，空闲连按两次退出\n"
            "Shift+Tab 切换权限；Ctrl+O 展开工具；PgUp/PgDn 滚动；拖选复制\n"
            "/new 新会话 · /compact 压缩上下文 · /help 帮助 · /exit 退出");
        else if (text == "/compact") {
            busy(true); phase_ = "压缩上下文"; activity();
            turn_stop_ = std::stop_source{};
            const auto token = turn_stop_.get_token();
            jobs_.push([this, token] {
                const auto status = agent_->compact([this](const agent::Event& event) {
                    rt_.post([this, event] { if (!exiting_) apply(event); });
                }, token);
                rt_.post([this, status] {
                    if (exiting_) return;
                    if (status == agent::TurnStatus::interrupted) notice("上下文压缩已取消");
                    busy(false); drain();
                });
            });
        }
        else if (text == "/new") {
            busy(true);
            jobs_.push([this] {
                auto setup = setup_;
                {
                    std::lock_guard lock(agent_mutex_);
                    setup.permission_mode = mode_;
                }
                auto next = agent::Agent::create(std::move(setup));
                std::string id = next->meta().id;
                {
                    // set_permission_mode 是 Agent 唯一允许跨线程的方法；换会话时保护对象寿命。
                    std::lock_guard lock(agent_mutex_);
                    next->set_permission_mode(mode_); agent_ = std::move(next);
                }
                rt_.post([this, id = std::move(id)] {
                    id_ = id; reset_transcript(); status_->session(setup_.model.model, id_);
                    busy(false); drain();
                });
            });
        } else notice("未知命令 " + text, tui::Notice::Severity::warn);
    }
    void interrupt() { if (busy_) turn_stop_.request_stop(); }
    void cancel() {
        if (busy_) { interrupt(); return; }
        if (!input_->text().empty()) { input_->set_text({}); return; }
        const auto now = Clock::now();
        if (last_cancel_ && now - *last_cancel_ < 1s) { exit(); return; }
        last_cancel_ = now; notice("再按一次 Ctrl+C 退出");
    }
    void cycle_permission() {
        std::lock_guard lock(agent_mutex_);
        mode_ = mode_ == agent::PermissionMode::ask ? agent::PermissionMode::accept_edits : agent::PermissionMode::ask;
        agent_->set_permission_mode(mode_); status_->permission(mode_);
    }
    void exit() {
        exiting_ = true; pending_.clear(); turn_stop_.request_stop(); jobs_.close();
        worker_.request_stop(); rt_.quit();
    }

    agent::Setup setup_;
    std::unique_ptr<agent::Agent> agent_;
    std::mutex agent_mutex_;
    agent::PermissionMode mode_ = agent::PermissionMode::ask;
    std::optional<ThemeSet> themes_;
    tui::ThemeTokens theme_ = tui::dark_theme();
    tui::Scrollback* scroll_ = nullptr;
    tui::Activity* activity_ = nullptr;
    tui::Notice* notice_ = nullptr;
    QueueLabel* queue_label_ = nullptr;
    tui::InputBox* input_ = nullptr;
    StatusLine* status_ = nullptr;
    tui::Terminal terminal_;
    tui::LayerStack root_;
    tui::Runtime rt_;
    Transcript transcript_;
    PromptInput prompt_;
    tui::Keymap keys_;
    tui::ScrollbackMouse mouse_;
    ApprovalDialog dialog_;
    JobQueue jobs_;
    std::jthread worker_;
    std::stop_source turn_stop_;
    std::string id_, error_, phase_;
    std::deque<std::string> pending_;
    struct Running { std::string id, summary; Clock::time_point begin; };
    std::vector<Running> running_;
    Clock::time_point step_begin_{};
    std::optional<Clock::time_point> last_cancel_;
    tui::TimerId activity_timer_ = 0, notice_timer_ = 0;
    bool busy_ = false, exiting_ = false, signal_exit_ = false;
};
} // namespace

int run_interactive(agent::Setup setup, const InteractiveOptions& options, agent::Interrupts& interrupts) {
    setup.permission_mode = agent::PermissionMode::ask;
    std::optional<ThemeSet> themes;
    if (options.theme_file && !options.theme_file->empty()) themes = load_theme(*options.theme_file);
    std::vector<agent::Event> history;
    std::unique_ptr<agent::Agent> agent;
    if (options.resume_id || options.continue_last) {
        const auto prefix = options.resume_id ? std::optional<std::string_view>(*options.resume_id) : std::nullopt;
        const auto id = agent::resolve_session_id(setup.session, setup.project_root, prefix);
        agent = agent::Agent::resume(setup, id, [&](const agent::Event& e) { history.push_back(e); });
    } else agent = agent::Agent::create(setup);
    Shell shell(std::move(setup), std::move(agent), std::move(history), std::move(themes));
    return shell.run(options.initial_prompt, interrupts);
}
} // namespace dagent::ui
