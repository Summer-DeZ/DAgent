#include "tui/input.hpp"

#include <algorithm>
#include <iterator>

namespace dagent::tui {

namespace {

constexpr std::string_view k_paste_end = "\x1b[201~";

// 参数个数与取值上限：键序列至多两三个参数，SGR 鼠标三个；
// 上限之外仍完整消费到终止符，只是不再记录（§9.1：必须读完再丢）。
constexpr int k_max_params = 8;
constexpr int k_param_cap = 0xFFFF;

// pos 处 UTF-8 序列的完整长度；缓冲不足且前缀合法 → 0（等更多字节）；
// 非法（坏续字节 / 过长编码 / 代理区 / 越界）→ -1（丢首字节）。
int utf8_length(std::string_view s) noexcept {
    const auto b0 = static_cast<unsigned char>(s[0]);
    int len = 0;
    char32_t cp = 0;
    char32_t min_cp = 0;
    if (b0 < 0x80) return 1;
    if ((b0 & 0xE0) == 0xC0) {
        len = 2;
        cp = b0 & 0x1F;
        min_cp = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3;
        cp = b0 & 0x0F;
        min_cp = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4;
        cp = b0 & 0x07;
        min_cp = 0x10000;
    } else {
        return -1;
    }
    if (s.size() < static_cast<std::size_t>(len)) {
        for (std::size_t i = 1; i < s.size(); ++i) {
            if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return -1;
        }
        return 0;
    }
    for (int i = 1; i < len; ++i) {
        const auto b = static_cast<unsigned char>(s[static_cast<std::size_t>(i)]);
        if ((b & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min_cp || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return -1;
    return len;
}

// xterm modifier 参数：值 = 1 + 位集（bit0 shift / bit1 alt / bit2 ctrl）。
// 缺省参数（0 或 1）= 无修饰。
Mods mods_from_param(int p) noexcept {
    if (p < 1) return Mods::none;
    const int bits = p - 1;
    Mods m = Mods::none;
    if (bits & 1) m = m | Mods::shift;
    if (bits & 2) m = m | Mods::alt;
    if (bits & 4) m = m | Mods::ctrl;
    return m;
}

Event key_event(Key k, Mods m, std::string_view ch) {
    Event e;
    e.kind = Event::Kind::key;
    e.key = k;
    e.mods = m;
    e.text.assign(ch);
    return e;
}

// C0 控制字节 → 键事件。CR/LF 都算 enter（raw 模式下 Enter 发 CR、
// Ctrl-J 发 LF）；Ctrl-字母按 xterm 惯例用 text 携带字母、mods=ctrl 表达。
Event control_event(unsigned char b, bool alt) {
    const Mods base = alt ? Mods::alt : Mods::none;
    switch (b) {
    case 0x0D:
    case 0x0A:
        return key_event(Key::enter, base, {});
    case 0x09:
        return key_event(Key::tab, base, {});
    case 0x00:
        return key_event(Key::none, base | Mods::ctrl, " ");
    case 0x1C:
        return key_event(Key::none, base | Mods::ctrl, "\\");
    case 0x1D:
        return key_event(Key::none, base | Mods::ctrl, "]");
    case 0x1E:
        return key_event(Key::none, base | Mods::ctrl, "^");
    case 0x1F:
        return key_event(Key::none, base | Mods::ctrl, "_");
    default: {
        const char ch = static_cast<char>('a' + b - 1);
        return key_event(Key::none, base | Mods::ctrl, std::string_view(&ch, 1));
    }
    }
}

// \e[n~ 的功能键表（§9.1）。
Key key_from_tilde(int p) noexcept {
    switch (p) {
    case 1:
    case 7:
        return Key::home;
    case 2:
        return Key::insert;
    case 3:
        return Key::del;
    case 4:
    case 8:
        return Key::end;
    case 5:
        return Key::page_up;
    case 6:
        return Key::page_down;
    case 11: return Key::f1;
    case 12: return Key::f2;
    case 13: return Key::f3;
    case 14: return Key::f4;
    case 15: return Key::f5;
    case 17: return Key::f6;
    case 18: return Key::f7;
    case 19: return Key::f8;
    case 20: return Key::f9;
    case 21: return Key::f10;
    case 23: return Key::f11;
    case 24: return Key::f12;
    default:
        return Key::none;
    }
}

// SGR 1006 鼠标：\e[<Cb;Cx;Cy(M|m)。Cb 位 0-1 按钮、4/8/16 = shift/alt/ctrl、
// 32 = 移动、64 = 滚轮；坐标 1 基，解码为 0 基。M = 按下/拖拽/滚轮，m = 释放。
void decode_mouse(const int* params, int np, char final_, bool alt,
                  std::vector<Event>& out) {
    if (np < 3) return;
    const int cb = params[0];
    Event e;
    e.kind = Event::Kind::mouse;
    if (cb & 4) e.mods = e.mods | Mods::shift;
    if (cb & 8) e.mods = e.mods | Mods::alt;
    if (cb & 16) e.mods = e.mods | Mods::ctrl;
    if (alt) e.mods = e.mods | Mods::alt;
    if (cb & 64) {
        e.mouse.button = 4 + (cb & 3); // 4/5/6/7 = 滚轮上/下/左/右
    } else {
        e.mouse.button = (cb & 3) == 3 ? -1 : cb & 3; // 3 = 无键（悬停/旧式释放）
    }
    e.mouse.col = params[1] - 1;
    e.mouse.row = params[2] - 1;
    e.mouse.press = final_ == 'M';
    e.mouse.motion = (cb & 32) != 0;
    out.push_back(std::move(e));
}

// CSI 键盘终止符 → 键/焦点事件。识别不了的终止符静默丢弃。
void decode_csi_key(const int* params, int np, char final_, bool alt,
                    std::vector<Event>& out) {
    Mods mods = np >= 2 ? mods_from_param(params[1]) : Mods::none;
    if (alt) mods = mods | Mods::alt;

    switch (final_) {
    case 'A': out.push_back(key_event(Key::up, mods, {})); return;
    case 'B': out.push_back(key_event(Key::down, mods, {})); return;
    case 'C': out.push_back(key_event(Key::right, mods, {})); return;
    case 'D': out.push_back(key_event(Key::left, mods, {})); return;
    case 'H': out.push_back(key_event(Key::home, mods, {})); return;
    case 'F': out.push_back(key_event(Key::end, mods, {})); return;
    case 'Z':
        out.push_back(key_event(Key::tab, mods | Mods::shift, {}));
        return;
    case 'P': out.push_back(key_event(Key::f1, mods, {})); return;
    case 'Q': out.push_back(key_event(Key::f2, mods, {})); return;
    case 'R': out.push_back(key_event(Key::f3, mods, {})); return;
    case 'S': out.push_back(key_event(Key::f4, mods, {})); return;
    case 'I':
    case 'O': {
        Event e;
        e.kind = Event::Kind::focus;
        e.focus_gained = final_ == 'I';
        out.push_back(std::move(e));
        return;
    }
    case '~': {
        const Key k = key_from_tilde(np >= 1 ? params[0] : 0);
        if (k != Key::none) out.push_back(key_event(k, mods, {}));
        return;
    }
    default:
        return;
    }
}

struct SeqResult {
    std::size_t next = 0; // 消费到的位置
    bool enter_paste = false;
    bool wait = false; // 序列不完整：一个字节都没消费、一个事件都没产出，留在缓冲等后续
};

// 序列内的非参数字节（CSI 与 SS3 共用，ECMA-48）：
//   C0 → 照常执行（产出事件，不带序列的 Alt 前缀），序列继续；
//   DEL → 忽略；≥0x80 → 中止序列。
enum class SeqByte { handled, abort, final_ };

SeqByte seq_control_byte(unsigned char c, std::vector<Event>& out) {
    if (c < 0x20) {
        out.push_back(control_event(c, false));
        return SeqByte::handled;
    }
    if (c == 0x7F) return SeqByte::handled;
    if (c >= 0x80) return SeqByte::abort;
    return SeqByte::final_;
}

// 不完整返回：撤回扫描期间已产出的 C0 事件 —— 缓冲原样保留，下一轮
// 会整体重新解析，不撤回就会重复产出（分块不变性）。
SeqResult wait_from(std::size_t start, std::vector<Event>& out, std::size_t base) {
    out.erase(out.begin() + static_cast<std::ptrdiff_t>(base), out.end());
    return {start, false, true};
}

// 从 start（\e[ 之后）扫描 CSI 序列到终止符。参数含 ';' 与数字；
// '<' 引导 SGR 鼠标；私有标记/中间字节/':' 使序列按不可识别处理 ——
// 仍完整读到终止符再整体丢弃；序列中途出现 ESC 时丢弃已收前缀并从
// 新 ESC 重新解析。
SeqResult decode_csi(std::string_view s, std::size_t start, bool alt,
                     std::vector<Event>& out) {
    int params[k_max_params] = {};
    int np = 0;
    int cur = 0;
    bool digit = false;
    bool sgr = false;
    bool junk = false;
    const std::size_t n = s.size();
    const std::size_t base = out.size();
    std::size_t i = start;

    const auto push = [&] {
        if (np < k_max_params) params[np++] = cur;
        cur = 0;
        digit = false;
    };

    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            if (cur > k_param_cap) cur = k_param_cap;
            digit = true;
            ++i;
            continue;
        }
        if (c == ';') {
            push();
            ++i;
            continue;
        }
        if (c == ':') {
            junk = true; // 子参数语法：不识别
            ++i;
            continue;
        }
        if (c == '<') {
            sgr = true;
            ++i;
            continue;
        }
        if ((c >= 0x3C && c <= 0x3F) || (c >= 0x20 && c <= 0x2F)) {
            junk = true; // 其余私有标记与中间字节
            ++i;
            continue;
        }
        if (c == 0x1B) return {i, false, false}; // 新 ESC 打断：前缀作废，重解析
        const SeqByte kind = seq_control_byte(c, out);
        if (kind == SeqByte::handled) {
            ++i;
            continue;
        }
        if (kind == SeqByte::abort) {
            return {i, false, false}; // 非 ASCII：中止序列，字节按 ground 重解析
        }
        // 0x40..0x7E：终止符
        if (digit || np > 0) push();
        ++i;
        if (junk) return {i, false, false};
        if (sgr) {
            decode_mouse(params, np, static_cast<char>(c), alt, out);
            return {i, false, false};
        }
        if (c == 'M' && np == 0) {
            // 旧式 X10 鼠标：\e[M 后跟 3 个原始字节。终端不支持 1006 时
            // 会发这种序列（terminal 按能力开启，老 tmux/screen 例外）；
            // 不解释该协议，但必须连负载一起吞掉，否则 3 个字节会被
            // 当成文本插进输入框。
            if (n - i < 3) return wait_from(start, out, base); // 负载不足：等待
            return {i + 3, false, false};
        }
        if (c == '~' && np >= 1) {
            if (params[0] == 200) return {i, true, false};  // 粘贴开始
            if (params[0] == 201) return {i, false, false}; // 游离的结束标记
        }
        decode_csi_key(params, np, static_cast<char>(c), alt, out);
        return {i, false, false};
    }
    return wait_from(start, out, base); // 不完整：留在缓冲等更多字节
}

// SS3（应用键盘模式）：\eO 后至多一段参数，单终止符。序列内的
// C0 / DEL / ≥0x80 与 CSI 同规则。
SeqResult decode_ss3(std::string_view s, std::size_t start, bool alt,
                     std::vector<Event>& out) {
    const std::size_t n = s.size();
    const std::size_t base = out.size();
    std::size_t i = start;
    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c <= 0x3F) {
            ++i; // 吸收参数与中间字节
            continue;
        }
        if (c == 0x1B) return {i, false, false};
        const SeqByte kind = seq_control_byte(c, out);
        if (kind == SeqByte::handled) {
            ++i;
            continue;
        }
        if (kind == SeqByte::abort) return {i, false, false};
        ++i;
        Mods mods = alt ? Mods::alt : Mods::none;
        switch (c) {
        case 'A': out.push_back(key_event(Key::up, mods, {})); break;
        case 'B': out.push_back(key_event(Key::down, mods, {})); break;
        case 'C': out.push_back(key_event(Key::right, mods, {})); break;
        case 'D': out.push_back(key_event(Key::left, mods, {})); break;
        case 'H': out.push_back(key_event(Key::home, mods, {})); break;
        case 'F': out.push_back(key_event(Key::end, mods, {})); break;
        case 'P': out.push_back(key_event(Key::f1, mods, {})); break;
        case 'Q': out.push_back(key_event(Key::f2, mods, {})); break;
        case 'R': out.push_back(key_event(Key::f3, mods, {})); break;
        case 'S': out.push_back(key_event(Key::f4, mods, {})); break;
        case 'M': out.push_back(key_event(Key::enter, mods, {})); break;
        default:
            break; // 识别不了：整体丢弃
        }
        return {i, false, false};
    }
    return wait_from(start, out, base);
}

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
        // 同一次 feed 内连续的可打印字符合并为一个 text 事件：大段输入
        // （不支持 2004 时的粘贴、输入法整段上屏）只触发一次
        // InputBox::insert，而不是逐码点 O(行长) 重算行宽。
        // 分块不变性因此约束"拼接后的文本"而不是事件边界。
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
                // ESC 串 = [多出的 Esc 按键…] [Alt 前缀] + 后随内容。
                //   * 后随 [ / O（CSI/SS3）：串的最后一个 ESC 是序列自身的
                //     引导符，倒数第二个是 Alt 前缀（rxvt 的 \e\e[A = Alt-Up），
                //     再往前的各是一次 Esc 按键；
                //   * 后随其他字节：最后一个 ESC 是 Alt 前缀，其余各是 Esc。
                // 串延伸到缓冲末尾时等后续字节或 flush_escape 消解。
                std::size_t esc = pos;
                while (esc < n && buf_[esc] == '\x1b') ++esc;
                if (esc == n) break;
                const std::size_t esc_run = esc - pos;
                const bool has_alt = esc_run >= 2;
                const auto c = static_cast<unsigned char>(buf_[esc]);
                flush_run();
                // 多余的 Esc 按键要等序列确认完整后才产出：若序列不完整
                // 留在缓冲，下一轮会整体重新解析，先发就会重复。
                const auto emit_strays = [&] {
                    for (std::size_t k = 0; k + 1 < esc_run; ++k) {
                        out.push_back(key_event(Key::escape, Mods::none, {}));
                    }
                };

                if (c == '[' || c == 'O') {
                    const std::size_t base = out.size();
                    const SeqResult r = c == '['
                        ? decode_csi(buf_, esc + 1, has_alt, out)
                        : decode_ss3(buf_, esc + 1, has_alt, out);
                    if (r.wait) break; // 不完整：原样留在缓冲，什么都没产出
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

bool Decoder::pending_escape() const noexcept {
    // 覆盖所有以 ESC 开头的不完整序列：孤立 ESC 串、\e[ / \eO 引导符、
    // 已收了部分参数的残缺 CSI —— 它们都在等后续字节，而终端发 Alt-[
    // / Alt-O 时用户可能不再按任何键，超时判定必须能把它们消解掉。
    return !paste_ && !buf_.empty() && buf_[0] == '\x1b';
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
        // 残缺 CSI/SS3。序列内的 C0 在 feed 里随 wait 被撤回，这里照常产出。
        const bool x10 = body.starts_with("[M"); // 负载是原始数据，不含按键
        bool params = x10;
        for (const char ch : body.substr(1)) {
            const auto c = static_cast<unsigned char>(ch);
            if (c >= 0x20 && c != 0x7F) params = true;
        }
        if (!params) {
            // 引导符后只有 C0/DEL：用户按了 Alt-[ / Alt-O 后停手（可能
            // 紧跟着回车等控制键）。最后一个 ESC 是 Alt 前缀。
            escapes(run - 1);
            out.push_back(key_event(Key::none, Mods::alt, body.substr(0, 1)));
        } else {
            // 已收参数的残缺序列整体丢弃：最后一个 ESC 是引导符，
            // 倒数第二个（若有）是它的 Alt 前缀，与 feed 规则一致。
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
    // 按下标自顶向下：期间 push 追加到原栈顶之上，不在本次遍历范围；
    // push 引起的重分配不影响下标访问；pop 只置空槽。
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
    // 焦点/全局在调用时刻读取：栈上的处理器可能刚改过它们。
    if (focus_ != nullptr && focus_->on_event(e)) return true;
    return global_ != nullptr && global_->on_event(e);
}

// ---- InputBoxHandler ----

bool InputBoxHandler::on_event(const Event& e) {
    // 粘贴与逐字输入同路：insert 入口会规范化 \r\n 并丢弃控制符，
    // 粘贴里的换行只分行、不触发提交。
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
