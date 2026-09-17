#include "tui/app.hpp"

#include <cerrno>
#include <climits>
#include <unistd.h>

#include <fcntl.h>
#include <poll.h>

namespace dagent::tui {

Runtime::Runtime(Terminal& term, Widget& root, Options opt)
    : term_(term), root_(root), opt_(opt) {
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

// ---- 配置 ----

void Runtime::on_tick(std::function<bool()> fn) {
    tick_fn_ = std::move(fn);
    ticking_ = false;
    arm_tick();
}

// ---- 更新通道 ----

// 渲染线程上的应用代码（事件处理器 / tick 回调 / render）调用 post()
// 时直接执行 —— 此时控件树已归渲染线程所有，入队反而是绕路；主循环
// 会在调用返回后检查 dirty_，不需要唤醒。
// 其余线程：节点在锁外分配，临界区只有一次尾插（两个指针写），最坏
// 情况 O(1) —— 与渲染耗时、与已积压的队列长度都无关（§3.1）。
void Runtime::post(std::function<void()> fn) {
    if (on_render_thread()) {
        fn();
        note_changes();
        arm_tick();
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
    arm_tick();
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

void Runtime::arm_tick() noexcept {
    if (ticking_ || !tick_fn_) return;
    ticking_ = true;
    next_tick_ = Clock::now() + opt_.tick;
}

void Runtime::route_events() {
    if (events_.empty()) return;
    for (const Event& e : events_) {
        router_.route(e);
    }
    events_.clear();
    note_changes();
    arm_tick(); // 输入可能启动了动画（例如提交后开始转圈）
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
    if (ticking_) consider(next_tick_);
    if (esc_due_) consider(*esc_due_);
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
    ticking_ = false;
    arm_tick();

    while (!quit_.load(std::memory_order_acquire)) {
        pollfd fds[3] = {
            {STDIN_FILENO, POLLIN, 0},
            {term_.signal_fd(), POLLIN, 0},
            {wake_pipe_[0], POLLIN, 0},
        };
        const int timeout = poll_timeout(Clock::now());
        const int rc = ::poll(fds, 3, timeout);
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
        if (ticking_ && now >= next_tick_) {
            next_tick_ = now + opt_.tick;
            ticking_ = tick_fn_ && tick_fn_(); // false：动画结束，暂停 tick
            note_changes();                    // 没动画就不出帧
        }
        if (esc_due_ && now >= *esc_due_) {
            esc_due_.reset();
            decoder_.flush_escape(events_);
            route_events();
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

} // namespace dagent::tui
