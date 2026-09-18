#include "tui/runtime.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <string_view>
#include <unistd.h>

#include <fcntl.h>
#include <poll.h>

namespace dagent::tui {

namespace {

// 握手查询组：DECRQM 2026/2027、\e[?u、OSC 11、DA1（最后发，作应答哨兵）。
constexpr std::string_view k_handshake_queries =
    "\x1b[?2026$p"
    "\x1b[?2027$p"
    "\x1b[?u"
    "\x1b]11;?\x1b\\"
    "\x1b[c";

constexpr std::chrono::milliseconds k_handshake_timeout{1000};

std::optional<int> parse_uint(std::string_view s) {
    if (s.empty()) return std::nullopt;
    int v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
        if (v > 100000) return std::nullopt;
    }
    return v;
}

// DECRQM 应答 ?{mode};{value}$y；value 1/2 = 支持。
bool parse_decrqm(std::string_view body, int& mode, int& value) {
    if (!body.starts_with('?') || !body.ends_with("$y")) return false;
    body.remove_prefix(1);
    body.remove_suffix(2);
    const std::size_t semi = body.find(';');
    if (semi == std::string_view::npos) return false;
    const std::optional<int> m = parse_uint(body.substr(0, semi));
    const std::optional<int> v = parse_uint(body.substr(semi + 1));
    if (!m || !v) return false;
    mode = *m;
    value = *v;
    return true;
}

int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// OSC 11 应答 11;rgb:RR/GG/BB，每段按位数放大到 8 位。
bool parse_osc11_background(std::string_view body, Color& out) {
    if (!body.starts_with("11;")) return false;
    body.remove_prefix(3);
    if (body.starts_with("rgba:")) {
        body.remove_prefix(5);
    } else if (body.starts_with("rgb:")) {
        body.remove_prefix(4);
    } else {
        return false;
    }

    uint8_t comp[3] = {};
    for (int i = 0; i < 3; ++i) {
        const std::size_t slash = body.find('/');
        if (i < 2 && slash == std::string_view::npos) return false;
        const std::string_view part = i < 2 ? body.substr(0, slash) : body;
        if (part.empty() || part.size() > 4) return false;
        int v = 0;
        for (const char c : part) {
            const int d = hex_digit(c);
            if (d < 0) return false;
            v = v * 16 + d;
        }
        const int max = (1 << (4 * static_cast<int>(part.size()))) - 1;
        comp[i] = static_cast<uint8_t>(v * 255 / max);
        if (i < 2) body.remove_prefix(slash + 1);
    }
    out = Color::rgb(comp[0], comp[1], comp[2]);
    return true;
}

// w 是否是 root 或其子孙。
bool within(const Widget* w, const Widget* root) noexcept {
    for (; w != nullptr; w = w->parent()) {
        if (w == root) return true;
    }
    return false;
}

} // namespace

Runtime::Runtime(Terminal& term, Widget& root, Options opt)
    : term_(term), root_(root), opt_(opt),
      stack_(dynamic_cast<LayerStack*>(&root)) {
    int pipes[2];
    if (::pipe2(pipes, O_NONBLOCK | O_CLOEXEC) != 0) {
        // 唤醒管道：创建失败直接终止。
        std::terminate();
    }
    wake_pipe_[0] = pipes[0];
    wake_pipe_[1] = pipes[1];
}

Runtime::Runtime(Terminal& term, Widget& root) : Runtime(term, root, Options{}) {}

Runtime::~Runtime() {
    for (Task* node = inbox_head_; node != nullptr;) {
        Task* const next = node->next;
        delete node;
        node = next;
    }
    if (wake_pipe_[0] >= 0) ::close(wake_pipe_[0]);
    if (wake_pipe_[1] >= 0) ::close(wake_pipe_[1]);
}

// ---- 定时器 ----

TimerId Runtime::after(std::chrono::milliseconds delay, std::function<void()> fn) {
    const TimerId id = next_timer_++;
    timers_.emplace(id, TimerEntry{std::move(fn), {}, {}});
    schedule(id, Clock::now() + delay);
    return id;
}

TimerId Runtime::every(std::chrono::milliseconds period, std::function<bool()> fn) {
    const TimerId id = next_timer_++;
    timers_.emplace(id, TimerEntry{{}, std::move(fn), period});
    schedule(id, Clock::now() + period);
    return id;
}

void Runtime::cancel(TimerId id) {
    if (timers_.erase(id) == 0) return;
    if (timer_heap_.size() > 2 * timers_.size() + 64) {
        std::erase_if(timer_heap_, [this](const TimerSlot& t) {
            return !timers_.contains(t.id);
        });
        std::make_heap(timer_heap_.begin(), timer_heap_.end(), TimerLater{});
    }
}

void Runtime::schedule(TimerId id, Clock::time_point due) {
    timer_heap_.push_back({due, id});
    std::push_heap(timer_heap_.begin(), timer_heap_.end(), TimerLater{});
}

void Runtime::prune_timers() {
    while (!timer_heap_.empty() && !timers_.contains(timer_heap_.front().id)) {
        std::pop_heap(timer_heap_.begin(), timer_heap_.end(), TimerLater{});
        timer_heap_.pop_back();
    }
}

// 依序执行已到期定时器；周期定时器按周期对齐续排。
void Runtime::run_timers(Clock::time_point now) {
    fired_.clear();
    while (!timer_heap_.empty() && timer_heap_.front().due <= now) {
        std::pop_heap(timer_heap_.begin(), timer_heap_.end(), TimerLater{});
        fired_.push_back(timer_heap_.back());
        timer_heap_.pop_back();
    }
    for (const TimerSlot& slot : fired_) {
        auto it = timers_.find(slot.id);
        if (it == timers_.end()) continue; // 已取消
        if (it->second.period.count() == 0) {
            std::function<void()> fn = std::move(it->second.once);
            timers_.erase(it);
            fn();
            continue;
        }
        std::function<bool()> fn = std::move(it->second.repeat);
        const auto period = it->second.period;
        const bool keep = fn();
        it = timers_.find(slot.id); // 回调可能取消了自己
        if (it == timers_.end()) continue;
        if (!keep) {
            timers_.erase(it); // 返回 false：取消
            continue;
        }
        it->second.repeat = std::move(fn);
        // 按周期对齐推进，落后时从现在起算。
        const auto next = slot.due + period;
        schedule(slot.id, next > now ? next : now + period);
    }
    if (!fired_.empty()) note_changes();
}

// ---- 浮层 ----

uint32_t Runtime::open_overlay(std::unique_ptr<Widget> w, Placement p,
                               Point point, EventHandler* modal,
                               Widget* cursor_source) {
    if (stack_ == nullptr) std::terminate(); // root 必须是 LayerStack
    Widget* const widget = w.get();
    const uint32_t id = stack_->push(std::move(w), p, point);
    overlays_.push_back({id, widget, modal, cursor_source, cursor_source_});
    if (modal != nullptr) router_.push(*modal);
    if (cursor_source != nullptr) {
        cursor_source_ = cursor_source;
        dirty_ = true; // 光标来源变了，需重新定位
    }
    note_changes();
    return id;
}

void Runtime::close_overlay(uint32_t id) {
    for (auto it = overlays_.begin(); it != overlays_.end(); ++it) {
        if (it->id != id) continue;
        if (it->modal != nullptr) router_.pop(*it->modal);
        if (it->cursor_source != nullptr) {
            // 仅当本浮层的光标来源仍生效时才恢复。
            if (cursor_source_ == it->cursor_source) {
                cursor_source_ = it->prev_cursor;
                dirty_ = true;
            }
            // 上层把本层光标来源记作恢复点的，改接本层的恢复点。
            for (auto up = it + 1; up != overlays_.end(); ++up) {
                if (up->prev_cursor == it->cursor_source) {
                    up->prev_cursor = it->prev_cursor;
                }
            }
        }
        // 浮层子树随即销毁：解除其中控件的鼠标绑定与捕获。
        const Widget* const root = it->widget;
        std::erase_if(mouse_bindings_, [&](const auto& b) {
            return within(b.first, root);
        });
        if (within(capture_.widget, root)) capture_ = {};
        stack_->remove(id);
        overlays_.erase(it);
        note_changes();
        return;
    }
}

// ---- 更新通道 ----

// 渲染线程上直接执行；其余线程入队后唤醒。
void Runtime::post(std::function<void()> fn) {
    if (on_render_thread()) {
        fn();
        note_changes();
        return;
    }
    Task* const node = new Task{std::move(fn), nullptr};
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        (inbox_tail_ != nullptr ? inbox_tail_->next : inbox_head_) = node;
        inbox_tail_ = node;
    }
    wake();
}

void Runtime::apply_inbox() {
    Task* node;
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        node = inbox_head_;
        inbox_head_ = nullptr;
        inbox_tail_ = nullptr;
    }
    if (node == nullptr) return;
    while (node != nullptr) {
        Task* const next = node->next;
        node->fn();
        delete node;
        node = next;
    }
    note_changes();
}

void Runtime::quit() noexcept {
    quit_.store(true, std::memory_order_release);
    wake();
}

// ---- 渲染线程 ----

bool Runtime::on_render_thread() const noexcept {
    return render_thread_.load(std::memory_order_acquire) == std::this_thread::get_id();
}

void Runtime::note_changes() noexcept {
    if (root_.dirty_tree() || root_.needs_layout()) dirty_ = true;
}

void Runtime::route_events() {
    if (events_.empty()) return;
    for (const Event& e : events_) {
        if (handle_handshake_reply(e)) continue;
        if (e.kind == Event::Kind::mouse) {
            dispatch_mouse(e); // 鼠标按坐标分发，键盘走路由链
        } else {
            router_.route(e);
        }
    }
    events_.clear();
    note_changes();
}

// ---- 鼠标命中 ----

void Runtime::bind_mouse(Widget& w, EventHandler& h) {
    for (auto& [widget, handler] : mouse_bindings_) {
        if (widget == &w) {
            handler = &h; // 重复绑定覆盖
            return;
        }
    }
    mouse_bindings_.push_back({&w, &h});
}

void Runtime::unbind_mouse(Widget& w) noexcept {
    std::erase_if(mouse_bindings_,
                  [&](const auto& b) { return b.first == &w; });
    if (capture_.widget == &w) capture_ = {}; // 解绑后不再投递捕获事件
}

EventHandler* Runtime::mouse_handler(Widget& w) const noexcept {
    for (const auto& [widget, handler] : mouse_bindings_) {
        if (widget == &w) return handler;
    }
    return nullptr;
}

// 坐标改写为相对命中控件（w 为空 = 屏幕坐标）后投递。
bool Runtime::deliver_mouse(EventHandler& h, const Widget* w, Event e) {
    if (w != nullptr) {
        const Point o = w->screen_origin();
        e.mouse.x = e.mouse.col - o.x;
        e.mouse.y = e.mouse.row - o.y;
    } else {
        e.mouse.x = e.mouse.col;
        e.mouse.y = e.mouse.row;
    }
    return h.on_event(e);
}

void Runtime::dispatch_mouse(const Event& event) {
    const Event::Mouse& m = event.mouse;

    // 1. 捕获：按下被消费后，该按钮的移动与释放直接交给捕获者。
    if (capture_.handler != nullptr && m.button == capture_.button &&
        (m.motion || !m.press)) {
        deliver_mouse(*capture_.handler, capture_.widget, event);
        if (!m.press) capture_ = {};
        return;
    }

    // 2. 模态：点在最上层模态浮层之外时事件交给模态处理器并吞掉。
    for (auto it = overlays_.rbegin(); it != overlays_.rend(); ++it) {
        if (it->modal == nullptr || it->widget == nullptr) continue;
        const Rect r = it->widget->screen_rect();
        if (!r.contains({m.col, m.row})) {
            Event out = event;
            out.mouse.outside = true;
            out.mouse.x = m.col - r.x;
            out.mouse.y = m.row - r.y;
            it->modal->on_event(out);
            return;
        }
        break; // 包含该点：继续走命中链
    }

    // 3. 命中链：命中控件沿父链向上，第一个绑定并消费的为止。
    if (stack_ != nullptr) {
        for (Widget* w = stack_->hit({m.col, m.row}); w != nullptr;
             w = w->parent()) {
            EventHandler* h = mouse_handler(*w);
            if (h == nullptr) continue;
            if (deliver_mouse(*h, w, event)) {
                // 非滚轮按下被消费：进入捕获。
                if (capture_.handler == nullptr && m.press && m.button >= 0 &&
                    m.button < 4) {
                    capture_ = {w, h, m.button};
                }
                return;
            }
        }
    }

    // 4. 全局处理器。
    if (global_ != nullptr) {
        deliver_mouse(*global_, nullptr, event);
    }
}

// ---- 能力握手 ----

// 发出握手查询，打开 1 秒应答窗口。
void Runtime::start_handshake() {
    if (!term_.can_query()) return;
    pending_caps_ = term_.caps();
    handshake_active_ = true;
    reply_due_ = Clock::now() + k_handshake_timeout;
    decoder_.set_reply_window(true);
    term_.write(k_handshake_queries);
}

// 消费窗口期内的握手应答；DA1 到达即提交。
bool Runtime::handle_handshake_reply(const Event& e) {
    if (!handshake_active_ || e.kind != Event::Kind::reply) return false;
    if (e.reply_type == Event::ReplyType::csi) {
        if (e.text.ends_with('c')) {
            finish_handshake(true); // DA1 哨兵
            return true;
        }
        if (e.text.ends_with("$y")) {
            int mode = 0;
            int value = 0;
            if (parse_decrqm(e.text, mode, value)) {
                // value 1/2 = 支持，其余不支持。
                const bool supported = value == 1 || value == 2;
                if (mode == 2026) pending_caps_.synchronized = supported;
                if (mode == 2027) pending_caps_.grapheme_width = supported;
            }
            return true;
        }
        if (e.text.ends_with('u')) {
            // 有应答即支持本协议。
            pending_caps_.kitty_keyboard = true;
            return true;
        }
        return true;
    }
    if (e.reply_type == Event::ReplyType::osc) {
        if (Color bg; parse_osc11_background(e.text, bg)) {
            pending_caps_.background = bg;
        }
        return true;
    }
    return true; // dcs/apc：握手期间一并消费
}

/// @brief commit=true 应用能力并推入终端模式；false = 超时放弃。
void Runtime::finish_handshake(bool commit) {
    if (!handshake_active_) return;
    handshake_active_ = false;
    reply_due_.reset();
    decoder_.set_reply_window(false); // 未终止的应答整体丢弃
    if (commit) {
        term_.apply_caps(pending_caps_);
        if (pending_caps_.kitty_keyboard) term_.set_kitty_keyboard(true);
        if (pending_caps_.grapheme_width) term_.set_grapheme_width(true);
    }
    pending_caps_ = Terminal::Caps{};
    report_caps();
}

void Runtime::on_caps(std::function<void(const Terminal::Caps&)> fn) {
    caps_fn_ = std::move(fn);
    if (caps_final_ && caps_fn_) caps_fn_(term_.caps());
}

/// @brief 通知能力回调并检查失效。
void Runtime::report_caps() {
    caps_final_ = true;
    if (caps_fn_) {
        caps_fn_(term_.caps());
        note_changes();
    }
}

void Runtime::check_size() {
    const Size now = term_.size();
    if (now == size_) return;
    size_ = now;
    back_.resize(now.cols, now.rows);
    root_.invalidate_tree(); // 终端上仍是旧内容，整树补画
    dirty_ = true;

    Event e;
    e.kind = Event::Kind::resize;
    e.size = now;
    router_.route(e); // 处理器的 invalidate 在本帧生效
}

void Runtime::frame() {
    check_size();
    const Rect area{0, 0, size_.cols, size_.rows};
    if (root_.needs_layout() || !(root_.rect() == area)) {
        root_.layout(area);
    }

    back_.copy_from(front_); // back 起点 = front
    root_.render(back_);     // 跳过干净子树

    cursor_.reset();
    if (cursor_source_ != nullptr) {
        if (const auto c = cursor_source_->cursor()) {
            // cursor() 是自身坐标，需沿父链累加到屏幕坐标。
            const Point o = cursor_source_->screen_origin();
            cursor_ = Point{o.x + c->x, o.y + c->y};
        }
    }
    dirty_ = false;
}

int Runtime::poll_timeout(Clock::time_point now) const noexcept {
    std::optional<Clock::time_point> due;
    const auto consider = [&](Clock::time_point t) {
        if (!due || t < *due) due = t;
    };
    if (!timer_heap_.empty()) consider(timer_heap_.front().due); // 已先 prune_timers
    if (esc_due_) consider(*esc_due_);
    if (reply_due_) consider(*reply_due_); // 握手超时
    if (dirty_) consider(last_frame_ + opt_.min_frame); // 合帧
    if (!due) return -1; // 无事可等：无限期阻塞
    if (*due <= now) return 0;
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(*due - now).count();
    return ms > INT_MAX ? INT_MAX : static_cast<int>(ms);
}

/// @brief 写唤醒字节；已有未消费字节则跳过。
void Runtime::wake() noexcept {
    if (wake_pending_.exchange(true, std::memory_order_acq_rel)) return;
    const char b = 0;
    ssize_t n;
    do {
        n = ::write(wake_pipe_[1], &b, 1);
    } while (n < 0 && errno == EINTR);
    // 管道满：唤醒已挂起。
}

void Runtime::drain_wake() noexcept {
    wake_pending_.store(false, std::memory_order_release);
    char buf[64];
    for (;;) {
        const ssize_t n = ::read(wake_pipe_[0], buf, sizeof buf);
        if (n > 0 || (n < 0 && errno == EINTR)) continue;
        break; // EAGAIN = 已读尽
    }
}

void Runtime::run() {
    render_thread_.store(std::this_thread::get_id(), std::memory_order_release);
    struct RenderThreadScope {
        Runtime& rt;
        ~RenderThreadScope() {
            rt.render_thread_.store(std::thread::id{}, std::memory_order_release);
        }
    } scope{*this};

    last_frame_ = Clock::now() - opt_.min_frame; // 首帧立即可出
    dirty_ = true;
    caps_final_ = false;
    start_handshake(); // 应答异步到达，不阻塞首帧
    if (!handshake_active_) report_caps(); // 不握手：初始值即最终值

    while (!quit_.load(std::memory_order_acquire)) {
        pollfd fds[3] = {
            {STDIN_FILENO, POLLIN, 0},
            {term_.signal_fd(), POLLIN, 0},
            {wake_pipe_[0], POLLIN, 0},
        };
        prune_timers();
        const int timeout = poll_timeout(Clock::now());
        const int rc = ::poll(fds, 3, timeout);
        wakeups_.fetch_add(1, std::memory_order_relaxed);
        if (rc < 0) {
            if (errno == EINTR) continue;
            quit_.store(true, std::memory_order_release); // poll 失败：收摊
            break;
        }

        bool resized = false;
        if ((fds[1].revents & POLLIN) != 0) {
            const Terminal::Signals sig = term_.drain_signal();
            // SIGINT/SIGTERM/SIGHUP → 退出；SIGWINCH → 查尺寸。
            if (sig.quit) quit_.store(true, std::memory_order_release);
            resized = sig.resize;
        }
        if ((fds[2].revents & POLLIN) != 0) drain_wake();

        // POLLNVAL：不消费会使 poll 立即返回空转。
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
            char buf[4096];
            ssize_t nread;
            do {
                nread = ::read(STDIN_FILENO, buf, sizeof buf);
            } while (nread < 0 && errno == EINTR);
            if (nread == 0 || (nread < 0 && errno != EAGAIN)) {
                // stdin 关闭/失效：无可交互，正常退出。
                quit_.store(true, std::memory_order_release);
            } else if (nread > 0) {
                decoder_.feed(std::string_view{buf, static_cast<std::size_t>(nread)},
                              events_);
                route_events();
                esc_due_ = decoder_.pending_escape()
                               ? std::optional(Clock::now() +
                                               std::chrono::milliseconds(
                                                   Decoder::k_escape_timeout_ms))
                               : std::nullopt;
            }
        }

        apply_inbox(); // 出帧前执行业务线程更新

        const auto now = Clock::now();
        if (resized) check_size();
        run_timers(now); // 先于出帧执行
        if (esc_due_ && now >= *esc_due_) {
            esc_due_.reset();
            decoder_.flush_escape(events_);
            route_events();
        }
        if (reply_due_ && now >= *reply_due_) {
            // 超时未收到 DA1：放弃握手。
            finish_handshake(false);
        }
        if (quit_.load(std::memory_order_acquire)) break;
        if (!dirty_) continue;
        if (now < last_frame_ + opt_.min_frame) continue; // 合帧

        frame();

        present(term_, back_, front_, out_, cursor_);
        frames_.fetch_add(1, std::memory_order_relaxed);
        last_frame_ = Clock::now();

        // intern 溢出：作废双缓冲，下一帧全量重写。
        if (intern_overflowed()) {
            intern_reset();
            front_.resize(0, 0);
            back_.clear();
            root_.invalidate_tree();
            dirty_ = true;
        }
    }
}

// ---- 挂起/恢复 ----

void Runtime::run_external(std::function<void()> fn) {
    term_.suspend();
    fn();
    resume_after_suspend();
}

void Runtime::suspend_process() {
    term_.suspend();
    ::kill(0, SIGTSTP);
    resume_after_suspend();
}

/// @brief 恢复后作废双缓冲，整树补画。
void Runtime::resume_after_suspend() {
    term_.resume();
    front_.resize(0, 0);
    root_.invalidate_tree();
    check_size(); // 挂起期间终端尺寸可能变过
    dirty_ = true;
}

// ---- ScrollbackMouse ----

ScrollbackMouse::~ScrollbackMouse() {
    if (click_timer_ != 0) rt_.cancel(click_timer_);
}

bool ScrollbackMouse::on_event(const Event& e) {
    if (e.kind != Event::Kind::mouse) return false;
    const Event::Mouse& m = e.mouse;
    if (m.button == 4 || m.button == 5) { // 滚轮
        sb_.scroll_lines(m.button == 4 ? -3 : 3);
        return true;
    }
    if (m.button != 0) return false;
    const Point at{m.x, m.y};

    if (m.press && !m.motion) {
        // 连击：窗口内同一格再按记为多击。
        clicks_ = click_timer_ != 0 && at == last_press_ ? clicks_ + 1 : 1;
        last_press_ = at;
        if (click_timer_ != 0) rt_.cancel(click_timer_);
        click_timer_ = rt_.after(k_multi_click, [this] {
            click_timer_ = 0;
            clicks_ = 0;
        });
        const std::optional<Location> loc = sb_.hit(at);
        dragging_ = false;
        if (!loc) return true;
        if (clicks_ == 1) {
            press_ = *loc;
            dragging_ = true;
            sb_.clear_selection();
        } else if (clicks_ == 2) {
            sb_.select(sb_.document().word_around(*loc));
        } else {
            sb_.select(sb_.document().line_around(*loc));
        }
        return true;
    }
    if (m.press && m.motion) { // 拖拽（捕获中，坐标可能在控件外）
        if (!dragging_) return true;
        if (const std::optional<Location> loc = sb_.hit(at)) {
            sb_.select({press_, *loc});
        }
        return true;
    }
    // 释放：复制选区。
    dragging_ = false;
    if (copy_on_release && sb_.selection()) rt_.set_clipboard(sb_.selected_text());
    return true;
}

} // namespace dagent::tui
