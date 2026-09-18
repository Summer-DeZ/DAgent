#include "tui/app.hpp"

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

// 握手查询组（§3.3）：DA1 必须最后发 —— 终端按顺序应答（DA1 之前没
// 收到的查询视为不支持），它同时是应答窗口的哨兵。
//   * DECRQM 2026 / 2027：同步输出、字素簇宽度；
//   * \e[?u：kitty 键盘协议当前 flags；
//   * OSC 11：背景色（ST 终止）；
//   * \e[c：DA1。
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

// DECRQM 应答文本：?{mode};{value}$y（value 1/2 = 已置位/已复位 = 支持）。
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

// OSC 11 应答文本：11;rgb:RR/GG/BB（每段 1..4 位十六进制，可同用 rgba:）。
// 每段按位数线性放大到 8 位，够 §3.12 判亮度。
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

// w 是否是 root 或其子孙（沿父链上溯）。
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
        // 唤醒管道是调度机制的支柱，创建失败直接终止（与 OOM 同级）。
        std::terminate();
    }
    wake_pipe_[0] = pipes[0];
    wake_pipe_[1] = pipes[1];
}

Runtime::Runtime(Terminal& term, Widget& root) : Runtime(term, root, Options{}) {}

Runtime::~Runtime() {
    // run() 退出后队列里可能还剩没跑的更新（业务线程在 quit 之后仍 post）。
    for (Task* node = inbox_head_; node != nullptr;) {
        Task* const next = node->next;
        delete node;
        node = next;
    }
    if (wake_pipe_[0] >= 0) ::close(wake_pipe_[0]);
    if (wake_pipe_[1] >= 0) ::close(wake_pipe_[1]);
}

// ---- 定时器（§3.9）----

// 到期时刻存最小堆；取消只从登记表删除，堆里的旧条目弹出时丢弃
// （惰性删除）。取消积压过多时整堆重建，堆大小与存活定时器同阶。
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

// 堆顶的已取消条目出堆：poll 超时只看存活定时器，取消不会造成空唤醒。
void Runtime::prune_timers() {
    while (!timer_heap_.empty() && !timers_.contains(timer_heap_.front().id)) {
        std::pop_heap(timer_heap_.begin(), timer_heap_.end(), TimerLater{});
        timer_heap_.pop_back();
    }
}

// 先摘出全部已到期条目再执行：回调里新建的 after(0) 留到下一轮，
// 不会在同一轮里无限续命。同时到期的按到期时刻、再按创建顺序执行。
// 回调执行前把函数移出登记表，回调可以安全地取消自己或别的定时器。
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
        // 按周期对齐推进；落后（慢帧）时从现在起算，不补发积压的周期。
        const auto next = slot.due + period;
        schedule(slot.id, next > now ? next : now + period);
    }
    if (!fired_.empty()) note_changes();
}

// ---- 浮层（§3.4.3）----

uint32_t Runtime::open_overlay(std::unique_ptr<Widget> w, Placement p,
                               Point point, EventHandler* modal,
                               Widget* cursor_source) {
    if (stack_ == nullptr) std::terminate(); // root 必须是 LayerStack（§3.4）
    Widget* const widget = w.get();
    const uint32_t id = stack_->push(std::move(w), p, point);
    overlays_.push_back({id, widget, modal, cursor_source, cursor_source_});
    if (modal != nullptr) router_.push(*modal);
    if (cursor_source != nullptr) {
        cursor_source_ = cursor_source;
        dirty_ = true; // 光标来源变了：即使控件都没失效也要重新定位
    }
    note_changes();
    return id;
}

void Runtime::close_overlay(uint32_t id) {
    for (auto it = overlays_.begin(); it != overlays_.end(); ++it) {
        if (it->id != id) continue;
        if (it->modal != nullptr) router_.pop(*it->modal);
        if (it->cursor_source != nullptr) {
            // 只在本浮层的光标来源仍生效时恢复，非后进先出的关闭不会
            // 把上层浮层的光标来源改掉。
            if (cursor_source_ == it->cursor_source) {
                cursor_source_ = it->prev_cursor;
                dirty_ = true;
            }
            // 上层浮层把本浮层的光标来源记作恢复点，而本浮层的控件随即
            // 销毁：恢复点改接到本浮层自己的恢复点，否则上层关闭时会
            // 恢复成悬垂指针。
            for (auto up = it + 1; up != overlays_.end(); ++up) {
                if (up->prev_cursor == it->cursor_source) {
                    up->prev_cursor = it->prev_cursor;
                }
            }
        }
        // 浮层子树随即销毁：解除其中控件的鼠标绑定与捕获（§3.5），否则
        // 拖拽中途关闭时后续移动/释放会交给已销毁的控件，旧绑定也会被
        // 复用同一地址的新控件继承。
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

// 渲染线程上的应用代码（事件处理器 / 定时器回调 / render）调用 post()
// 时直接执行 —— 此时控件树已归渲染线程所有，入队反而是绕路；主循环
// 会在调用返回后检查 dirty_，不需要唤醒。
// 其余线程：节点在锁外分配，临界区只有一次尾插（两个指针写），最坏
// 情况 O(1) —— 与渲染耗时、与已积压的队列长度都无关（§3.1）。
void Runtime::post(std::function<void()> fn) {
    if (on_render_thread()) {
        fn();
        note_changes();
        return;
    }
    Task* const node = new Task{std::move(fn), nullptr};
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        // 空链表接头，否则接在尾节点后面。
        (inbox_tail_ != nullptr ? inbox_tail_->next : inbox_head_) = node;
        inbox_tail_ = node;
    }
    wake();
}

// 渲染线程每次被唤醒的固定动作：锁内只把整条链摘下来（头尾置空）；
// 解锁后依序执行并释放节点，最后检查控件树是否失效。执行期间新到的
// post 接在已空的链上并重新唤醒，不丢。
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
            dispatch_mouse(e); // 键盘走路由链，鼠标按坐标分发（§3.5）
        } else {
            router_.route(e);
        }
    }
    events_.clear();
    note_changes();
}

// ---- 鼠标命中（§3.5）----

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

// 事件先改写为相对命中控件的坐标（w 为空 = 全局：保持屏幕坐标）。
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

    // 1. 捕获：按下被消费后，该按钮的移动与释放都直接交给它，
    //    不论指针是否移出（拖拽选择、拖动滚动条）。
    if (capture_.handler != nullptr && m.button == capture_.button &&
        (m.motion || !m.press)) {
        deliver_mouse(*capture_.handler, capture_.widget, event);
        if (!m.press) capture_ = {};
        return;
    }

    // 2. 模态：带 modal 的最上层浮层。点在它之外 → 事件给模态处理器
    //    （outside = true）并吞掉，不穿透到下层；是否关闭由处理器决定。
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
        break; // 包含该点：继续走命中链（模态处理器可绑定在浮层上）
    }

    // 3. 命中链：最上层包含该点的最深控件沿父链向上，第一个绑定了
    //    处理器且消费的为止。
    if (stack_ != nullptr) {
        for (Widget* w = stack_->hit({m.col, m.row}); w != nullptr;
             w = w->parent()) {
            EventHandler* h = mouse_handler(*w);
            if (h == nullptr) continue;
            if (deliver_mouse(*h, w, event)) {
                // 非滚轮的按下被消费：进入捕获，后续拖拽/释放归它。
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

// ---- 能力握手（§3.3）----

// run() 开始时调用：发出查询、打开应答窗口、起 1 秒超时。查询只是
// 写出，不等待 —— 首帧照常调度，应答在 poll 循环里以事件到达。
// pending_caps_ 从初始值出发，只被收到的应答覆盖，未应答的项不变。
// 非交互终端（stdin/stdout 被重定向）不握手，能力保持环境变量初始值。
void Runtime::start_handshake() {
    if (!term_.can_query()) return;
    pending_caps_ = term_.caps();
    handshake_active_ = true;
    reply_due_ = Clock::now() + k_handshake_timeout;
    decoder_.set_reply_window(true);
    term_.write(k_handshake_queries);
}

// 窗口期间所有应答都由运行时消费（应用只看到按键）。DA1（私有 CSI、
// 最终字节 c）是哨兵：收到即提交累积的能力；其余按应答类型记录。
bool Runtime::handle_handshake_reply(const Event& e) {
    if (!handshake_active_ || e.kind != Event::Kind::reply) return false;
    if (e.reply_type == Event::ReplyType::csi) {
        if (e.text.ends_with('c')) {
            finish_handshake(true); // 哨兵：终端按顺序应答，DA1 之后无需再等
            return true;
        }
        if (e.text.ends_with("$y")) {
            int mode = 0;
            int value = 0;
            if (parse_decrqm(e.text, mode, value)) {
                // value 1/2 = 已置位/已复位（支持），其余 = 不支持；
                // 有应答即以应答为准（可覆盖环境变量的乐观猜测）。
                const bool supported = value == 1 || value == 2;
                if (mode == 2026) pending_caps_.synchronized = supported;
                if (mode == 2027) pending_caps_.grapheme_width = supported;
            }
            return true;
        }
        if (e.text.ends_with('u')) {
            // 有应答即支持本协议；flag 1 由 set_kitty_keyboard 推入。
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

// commit = DA1 已到（提交能力，含推入 kitty flag 1）；false = 1 秒
// 超时（管道、异常终端），保持环境变量初始值。
void Runtime::finish_handshake(bool commit) {
    if (!handshake_active_) return;
    handshake_active_ = false;
    reply_due_.reset();
    decoder_.set_reply_window(false); // 未终止的应答整体丢弃
    if (commit) {
        term_.apply_caps(pending_caps_);
        if (pending_caps_.kitty_keyboard) term_.set_kitty_keyboard(true);
    }
    pending_caps_ = Terminal::Caps{};
}

void Runtime::check_size() {
    // ioctl 约 1µs（§四）。SIGWINCH 唤醒时与每帧开头各查一次。
    const Size now = term_.size();
    if (now == size_) return;
    size_ = now;
    back_.resize(now.cols, now.rows);
    root_.invalidate_tree(); // resize 纪元：终端上还是旧内容，整树补画
    dirty_ = true;

    Event e;
    e.kind = Event::Kind::resize;
    e.size = now;
    router_.route(e); // 处理器的 invalidate 会在本帧的布局/光栅化里生效
}

void Runtime::frame() {
    check_size();
    const Rect area{0, 0, size_.cols, size_.rows};
    if (root_.needs_layout() || !(root_.rect() == area)) {
        root_.layout(area);
    }

    back_.copy_from(front_); // back 起点 = front：未失效区域即终端真相
    root_.render(back_);     // Container 跳过干净子树（§七）

    cursor_.reset();
    if (cursor_source_ != nullptr) {
        if (const auto c = cursor_source_->cursor()) {
            // cursor() 是控件自身坐标；rect() 只是父容器局部坐标，嵌套时
            // 必须沿父链累加到屏幕坐标。
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
    if (reply_due_) consider(*reply_due_); // 握手超时：1 秒内没有 DA1 就收窗口
    if (dirty_) consider(last_frame_ + opt_.min_frame); // 合帧：余量交给 poll 精确等待
    if (!due) return -1; // 无事可等：无限期阻塞，静止界面零唤醒
    if (*due <= now) return 0;
    // 向上取整：向下截断会让 poll 提前醒来、在最后不足 1ms 里空转。
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(*due - now).count();
    return ms > INT_MAX ? INT_MAX : static_cast<int>(ms);
}

// 突发合并：管道里已有未消费的唤醒字节时不再写（一千次 post 一次 write）。
// post 先入队再唤醒；渲染线程先清标志、读管道，再交换队列 —— 清标志后
// 到达的 post 必然重新写入唤醒字节，且其 fn 必然还在 inbox_ 里等着被
// 交换，唤醒与数据都不会丢。
void Runtime::wake() noexcept {
    if (wake_pending_.exchange(true, std::memory_order_acq_rel)) return;
    const char b = 0;
    ssize_t n;
    do {
        n = ::write(wake_pipe_[1], &b, 1);
    } while (n < 0 && errno == EINTR);
    // EAGAIN：管道满 = 唤醒已挂起，无需再写。
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
    start_handshake(); // 查询已写出，应答到达前不阻塞首帧

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
            // SIGINT/SIGTERM/SIGHUP → 正常退出路径（§四）；SIGWINCH → 查尺寸。
            if (sig.quit) quit_.store(true, std::memory_order_release);
            resized = sig.resize;
        }
        if ((fds[2].revents & POLLIN) != 0) drain_wake();

        // POLLNVAL：stdin 不是有效描述符，poll 会立即返回 —— 不处理就空转。
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

        apply_inbox(); // 业务线程的领域更新在出帧前落到控件树上

        const auto now = Clock::now();
        if (resized) check_size();
        run_timers(now); // 回调失效了控件才出帧
        if (esc_due_ && now >= *esc_due_) {
            esc_due_.reset();
            decoder_.flush_escape(events_);
            route_events();
        }
        if (reply_due_ && now >= *reply_due_) {
            // 1 秒内没收到 DA1：窗口关闭，保持环境变量初始值。
            finish_handshake(false);
        }
        if (quit_.load(std::memory_order_acquire)) break;
        if (!dirty_) continue;
        if (now < last_frame_ + opt_.min_frame) continue; // 合帧余量已交给 poll

        frame();

        // 差分 + 写终端：渲染线程是唯一的终端 I/O 者，业务线程在队列
        // 另一侧，终端写入慢时只会让队列积压，不会阻塞业务线程。
        present(term_, back_, front_, out_, cursor_);
        frames_.fetch_add(1, std::memory_order_relaxed);
        last_frame_ = Clock::now();
    }
}

// ---- 挂起/恢复（§3.10）----

// 外部程序（$EDITOR 等）独占终端的窗口：挂起框架的界面模式，跑完再恢复。
// 本函数阻塞渲染线程，期间 post 在更新队列里累积，恢复后由主循环统一
// 执行；fn 直接向 stdout 写自己的内容（此时终端已不在备用屏）。
void Runtime::run_external(std::function<void()> fn) {
    term_.suspend();
    fn();
    resume_after_suspend();
}

// Ctrl+Z：raw 模式下以字节 0x1A 到达，由全局处理器决定调用。向整个进程
// 组发 SIGTSTP（与终端 Ctrl+Z 的效果一致，应用拉起的子进程一并停住），
// 收到 SIGCONT 后从这里继续（shell 的 fg/bg 会发 SIGCONT）；孤儿进程组
// 中内核丢弃 SIGTSTP，表现为立即恢复。
void Runtime::suspend_process() {
    term_.suspend();
    ::kill(0, SIGTSTP);
    resume_after_suspend();
}

// 恢复后的共同收尾：终端在挂起期间被外部程序改过，备用屏内容也不再由
// front_ 代表 —— 作废双缓冲、整树补画，下一帧走全量写出。
void Runtime::resume_after_suspend() {
    term_.resume();
    front_.resize(0, 0);
    root_.invalidate_tree();
    check_size(); // 挂起期间终端尺寸可能变过
    dirty_ = true;
}

// ---- ScrollbackMouse（§3.8）----

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
        // 连击：窗口内同一格再按一次记为多击；窗口从每次按下重新起算。
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
    // 释放：单击未拖动时选区已清除，不复制。
    dragging_ = false;
    if (copy_on_release && sb_.selection()) rt_.set_clipboard(sb_.selected_text());
    return true;
}

} // namespace dagent::tui
