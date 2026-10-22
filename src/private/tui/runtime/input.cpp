#include "tui/runtime/input.hpp"

#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/runtime/input_detail.hpp"

namespace dagent::tui {

using namespace input_detail;

namespace {

constexpr std::string_view k_paste_end = "\x1b[201~";

} // namespace

// ---- Decoder ----

void Decoder::feed(std::string_view bytes, std::vector<Event>& out) {
    buf_.append(bytes);

    for (;;) {
        if (paste_) {
            if (drain_paste(out)) return; // 仍在收集
            continue;                     // 粘贴收尾，剩余字节按 ground 解析
        }

        const std::size_t n = buf_.size();
        std::size_t pos = 0;
        // 连续可打印字符合并为一个 text 事件。
        std::string run;
        const auto flush_run = [&] {
            if (run.empty()) return;
            Event e;
            e.kind = Event::Kind::text;
            e.text = std::move(run);
            run.clear();
            out.push_back(std::move(e));
        };

        while (pos < n) {
            const auto b = static_cast<unsigned char>(buf_[pos]);

            if (b == 0x1B) {
                // ESC 串 = 多余的 Esc 按键 + Alt 前缀 + 后随内容。
                std::size_t esc = pos;
                while (esc < n && buf_[esc] == '\x1b') ++esc;
                if (esc == n) break;
                const std::size_t esc_run = esc - pos;
                const bool has_alt = esc_run >= 2;
                const auto c = static_cast<unsigned char>(buf_[esc]);
                flush_run();
                // 多余的 Esc 按键等序列确认完整后再产出。
                const auto emit_strays = [&] {
                    for (std::size_t k = 0; k + 1 < esc_run; ++k) {
                        out.push_back(key_event(Key::escape, Mods::none, {}));
                    }
                };

                if (c == '[' || c == 'O') {
                    const std::size_t base = out.size();
                    const SeqResult r = c == '['
                        ? decode_csi(buf_, esc + 1, has_alt, reply_window_, out)
                        : decode_ss3(buf_, esc + 1, has_alt, out);
                    if (r.wait) {
                        // 残缺应答不参与 Esc 超时
                        reply_pending_ = r.reply;
                        break;
                    }
                    reply_pending_ = false;
                    if (esc_run >= 3) {
                        out.insert(out.begin() + static_cast<std::ptrdiff_t>(base),
                                   esc_run - 2,
                                   key_event(Key::escape, Mods::none, {}));
                    }
                    pos = r.next;
                    if (c == '[' && r.enter_paste) {
                        paste_ = true;
                        paste_scanned_ = 0;
                        break; // 交给外层循环的 drain_paste
                    }
                    continue;
                }
                // OSC 11 背景变化也会在握手结束后主动上报。只放行背景应答，保留 Alt+]。
                bool background_reply = false;
                if (!reply_window_ && !has_alt && c == ']') {
                    const auto body = std::string_view(buf_).substr(esc + 1);
                    constexpr std::string_view rgb = "11;rgb:", rgba = "11;rgba:";
                    if (rgb.starts_with(body) || rgba.starts_with(body)) break;
                    background_reply = body.starts_with(rgb) || body.starts_with(rgba);
                }
                // 其余终端字符串只在应答窗口解析；多余的 ESC 是 Alt 组合键。
                if ((reply_window_ || background_reply) && !has_alt &&
                    (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X')) {
                    const SeqResult r =
                        decode_string(buf_, esc + 1, static_cast<char>(c), out);
                    if (r.wait) {
                        reply_pending_ = true;
                        break;
                    }
                    reply_pending_ = false;
                    pos = r.next;
                    continue;
                }
                if (c >= 0x80) {
                    // alt + 多字节字符：等整个单元凑齐。
                    const int len =
                        utf8_length(std::string_view(buf_).substr(esc));
                    if (len == 0) break;
                    emit_strays();
                    if (len > 0) {
                        out.push_back(key_event(
                            Key::none, Mods::alt,
                            std::string_view(buf_).substr(
                                esc, static_cast<std::size_t>(len))));
                        pos = esc + static_cast<std::size_t>(len);
                    } else {
                        pos = esc + 1; // 非法：只丢首字节（连同 ESC 前缀）
                    }
                    continue;
                }
                emit_strays();
                if (c == 0x7F) {
                    out.push_back(key_event(Key::backspace, Mods::alt, {}));
                } else if (c < 0x20) {
                    out.push_back(control_event(c, true));
                } else {
                    // ESC + 单个可打印字节：alt + 该字符
                    out.push_back(key_event(
                        Key::none, Mods::alt,
                        std::string_view(buf_).substr(esc, 1)));
                }
                pos = esc + 1;
                continue;
            }

            if (b == 0x7F) {
                flush_run();
                out.push_back(key_event(Key::backspace, Mods::none, {}));
                pos += 1;
                continue;
            }

            if (b < 0x20) {
                flush_run();
                out.push_back(control_event(b, false));
                pos += 1;
                continue;
            }

            const int len = utf8_length(std::string_view(buf_).substr(pos));
            if (len == 0) break; // 不完整：等待
            if (len < 0) {       // 非法：丢首字节，文本继续合并
                pos += 1;
                continue;
            }
            run.append(buf_, pos, static_cast<std::size_t>(len));
            pos += static_cast<std::size_t>(len);
        }
        flush_run();

        buf_.erase(0, pos);
        if (!paste_) return;
    }
}

void Decoder::set_reply_window(bool open) noexcept {
    reply_window_ = open;
    if (!open && reply_pending_) {
        // 窗口关闭时仍未终止的应答整体丢弃。
        buf_.clear();
        reply_pending_ = false;
    }
}

bool Decoder::pending_escape() const noexcept {
    // 以 ESC 开头的不完整序列均可被超时消解；应答窗口内的残缺应答除外。
    return !paste_ && !reply_pending_ && !buf_.empty() && buf_[0] == '\x1b';
}

void Decoder::flush_escape(std::vector<Event>& out) {
    if (!pending_escape()) return;
    std::size_t run = 0;
    while (run < buf_.size() && buf_[run] == '\x1b') ++run;
    const std::string_view body{buf_.data() + run, buf_.size() - run};
    const auto escapes = [&](std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(key_event(Key::escape, Mods::none, {}));
        }
    };

    // feed 停下时缓冲里只剩一个不完整单元，body 只可能是以下几种：
    if (body.empty()) {
        escapes(run); // 孤立 ESC 串：每个都是 Esc 按键
    } else if (body[0] == '[' || body[0] == 'O') {
        // 残缺 CSI/SS3：序列内 C0 已被 feed 撤回，这里照常产出。
        const bool x10 = body.starts_with("[M"); // 负载是原始数据，不含按键
        bool params = x10;
        for (const char ch : body.substr(1)) {
            const auto c = static_cast<unsigned char>(ch);
            if (c >= 0x20 && c != 0x7F) params = true;
        }
        if (!params) {
            // 引导符后只有 C0/DEL：最后一个 ESC 是 Alt 前缀。
            escapes(run - 1);
            out.push_back(key_event(Key::none, Mods::alt, body.substr(0, 1)));
        } else {
            // 已收参数的残缺序列整体丢弃：最后一个 ESC 是引导符，倒数第二个是 Alt 前缀。
            escapes(run >= 2 ? run - 2 : 0);
        }
        if (!x10) {
            for (const char ch : body.substr(1)) {
                const auto c = static_cast<unsigned char>(ch);
                if (c < 0x20) out.push_back(control_event(c, false));
            }
        }
    } else {
        // Alt + 不完整的多字节字符：最后一个 ESC 是 Alt 前缀，残缺字符丢弃。
        escapes(run - 1);
    }
    buf_.clear();
}

bool Decoder::drain_paste(std::vector<Event>& out) {
    // 增量查找结束标记：上次已排除的位置往前留一个标记长度 - 1 的重叠。
    constexpr std::size_t k_end_len = k_paste_end.size();
    const std::size_t from =
        paste_scanned_ >= k_end_len - 1 ? paste_scanned_ - (k_end_len - 1) : 0;
    const std::size_t hit = std::string_view(buf_).find(k_paste_end, from);
    if (hit == std::string_view::npos) {
        const std::size_t n = buf_.size();
        paste_scanned_ = n >= k_end_len - 1 ? n - (k_end_len - 1) : 0;
        return true;
    }
    Event e;
    e.kind = Event::Kind::paste;
    e.text.assign(buf_, 0, hit);
    out.push_back(std::move(e));
    buf_.erase(0, hit + k_end_len);
    paste_ = false;
    paste_scanned_ = 0;
    return false;
}

// ---- EventRouter ----

void EventRouter::push(EventHandler& h) { stack_.push_back(&h); }

void EventRouter::pop(EventHandler& h) {
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        if (*it != &h) continue;
        if (routing_ > 0) {
            // 下发中：只置空槽，保持下标稳定；空槽不再被调用。
            *it = nullptr;
            has_holes_ = true;
        } else {
            stack_.erase(std::next(it).base());
        }
        return;
    }
}

bool EventRouter::route(const Event& e) {
    // 按下标自顶向下；期间的 push/pop 不影响本次遍历。
    struct Depth {
        EventRouter& r;
        explicit Depth(EventRouter& router) : r(router) { ++r.routing_; }
        ~Depth() {
            if (--r.routing_ == 0 && r.has_holes_) {
                std::erase(r.stack_, nullptr);
                r.has_holes_ = false;
            }
        }
    } depth(*this);

    for (std::size_t i = stack_.size(); i-- > 0;) {
        EventHandler* h = stack_[i];
        if (h != nullptr && h->on_event(e)) return true;
    }
    // 焦点/全局在调用时刻读取。
    if (focus_ != nullptr && focus_->on_event(e)) return true;
    return global_ != nullptr && global_->on_event(e);
}

// ---- InputBoxHandler ----

bool InputBoxHandler::on_event(const Event& e) {
    // 粘贴与逐字输入同路走 insert。
    if (e.kind == Event::Kind::text || e.kind == Event::Kind::paste) {
        box_.insert(e.text);
        return true;
    }
    if (e.kind != Event::Kind::key || any(e.mods)) return false;
    switch (e.key) {
    case Key::backspace: box_.backspace(); return true;
    case Key::del: box_.del(); return true;
    case Key::left: box_.move(-1, 0); return true;
    case Key::right: box_.move(1, 0); return true;
    case Key::up: box_.move(0, -1); return true;
    case Key::down: box_.move(0, 1); return true;
    case Key::home: box_.line_home(); return true;
    case Key::end: box_.line_end(); return true;
    case Key::tab: box_.insert("\t"); return true;
    default:
        return false; // enter / escape / 功能键 → 应用策略（模态或全局处理器）
    }
}

} // namespace dagent::tui
