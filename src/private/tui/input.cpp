#include "tui/input.hpp"

#include <algorithm>
#include <iterator>

namespace dagent::tui {

namespace {

constexpr std::string_view k_paste_end = "\x1b[201~";

// 超限参数仍消费到终止符，只是不再记录。
constexpr int k_max_params = 8;
constexpr int k_param_cap = 0xFFFF;

// 超出部分丢弃但仍读到终止符。
constexpr std::size_t k_max_reply = 1u << 20;

// s[0] 处 UTF-8 序列长度：缓冲不足且前缀合法 → 0（等更多字节），非法 → -1（丢首字节）。
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

// modifier 参数 = 1 + 位集（shift/alt/ctrl/super）；缺省 = 无修饰。
Mods mods_from_param(int p) noexcept {
    if (p < 1) return Mods::none;
    const int bits = p - 1;
    Mods m = Mods::none;
    if (bits & 1) m = m | Mods::shift;
    if (bits & 2) m = m | Mods::alt;
    if (bits & 4) m = m | Mods::ctrl;
    if (bits & 8) m = m | Mods::super;
    return m;
}

bool valid_codepoint(int cp) noexcept {
    return cp > 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
}

// 仅对 valid_codepoint 的输入调用。
std::string utf8_from_codepoint(int cp) {
    std::string s;
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return s;
}

void push_reply(std::vector<Event>& out, Event::ReplyType type,
                std::string_view body) {
    Event e;
    e.kind = Event::Kind::reply;
    e.reply_type = type;
    e.text.assign(body);
    out.push_back(std::move(e));
}

Event key_event(Key k, Mods m, std::string_view ch) {
    Event e;
    e.kind = Event::Kind::key;
    e.key = k;
    e.mods = m;
    e.text.assign(ch);
    return e;
}

void push_text_key(std::vector<Event>& out, Mods mods, std::string_view text) {
    Event e;
    e.kind = any(mods) ? Event::Kind::key : Event::Kind::text;
    e.key = Key::none;
    e.mods = mods;
    e.text.assign(text);
    out.push_back(std::move(e));
}

// C0 控制字节 → 键事件；Ctrl-字母用 text 携带字母、mods=ctrl 表达。
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

// \e[n~ 的功能键表。
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

// SGR 1006 鼠标：坐标 1 基解码为 0 基，M = 按下/拖拽，m = 释放。
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

// CSI 参数：支持 ':' 子参数与私有标记（'?' '>' '=' 应答、'<' SGR 鼠标）。
struct CsiParams {
    int v[k_max_params] = {};         ///< 各 ';' 段的主值
    int sub[k_max_params] = {};       ///< 各段 ':' 后的第一个子参数
    bool sub_seen[k_max_params] = {}; ///< 该段是否带子参数
    int np = 0;
    char marker = 0;           ///< 私有标记（'?' '>' '='），0 = 无
    bool sgr = false;          ///< '<' 私有标记：SGR 鼠标
    bool intermediate = false; ///< 中间字节（如 DECRQM 的 '$'）
    bool junk = false;         ///< 其余不可识别的私有标记/中间字节
};

// kitty 键 \e[code;mods:event u：命名键映射，其余按可打印码点产出。
void decode_kitty_key(const CsiParams& p, bool alt, std::vector<Event>& out) {
    if (p.np < 1 || p.v[0] <= 0) return;
    if (p.sub_seen[1] && p.sub[1] == 3) return; // release 事件
    Mods mods = p.np >= 2 ? mods_from_param(p.v[1]) : Mods::none;
    if (alt) mods = mods | Mods::alt;
    int code = p.v[0];
    // 有 shift 且带 shifted key 时，alternate 才是实际上屏的字符。
    if (any(mods & Mods::shift) && p.sub_seen[0] && p.sub[0] > 0) {
        code = p.sub[0];
    }
    switch (code) {
    case 13: out.push_back(key_event(Key::enter, mods, {})); return;
    case 9: out.push_back(key_event(Key::tab, mods, {})); return;
    case 127: out.push_back(key_event(Key::backspace, mods, {})); return;
    case 27: out.push_back(key_event(Key::escape, mods, {})); return;
    case 57409: push_text_key(out, mods, "."); return; // KP_DECIMAL
    case 57410: push_text_key(out, mods, "/"); return; // KP_DIVIDE
    case 57411: push_text_key(out, mods, "*"); return; // KP_MULTIPLY
    case 57412: push_text_key(out, mods, "-"); return; // KP_SUBTRACT
    case 57413: push_text_key(out, mods, "+"); return; // KP_ADD
    case 57414: out.push_back(key_event(Key::enter, mods, {})); return;
    case 57415: push_text_key(out, mods, "="); return; // KP_EQUAL
    case 57416: push_text_key(out, mods, ","); return; // KP_SEPARATOR
    case 57417: out.push_back(key_event(Key::left, mods, {})); return;
    case 57418: out.push_back(key_event(Key::right, mods, {})); return;
    case 57419: out.push_back(key_event(Key::up, mods, {})); return;
    case 57420: out.push_back(key_event(Key::down, mods, {})); return;
    case 57421: out.push_back(key_event(Key::page_up, mods, {})); return;
    case 57422: out.push_back(key_event(Key::page_down, mods, {})); return;
    case 57423: out.push_back(key_event(Key::home, mods, {})); return;
    case 57424: out.push_back(key_event(Key::end, mods, {})); return;
    case 57425: out.push_back(key_event(Key::insert, mods, {})); return;
    case 57426: out.push_back(key_event(Key::del, mods, {})); return;
    default:
        break;
    }
    if (code >= 57399 && code <= 57408) {
        const char digit = static_cast<char>('0' + code - 57399);
        push_text_key(out, mods, std::string_view(&digit, 1));
        return;
    }
    // kitty 把功能键放在 Unicode 私用区；未识别的功能键不能当文本插入。
    if (code >= 57344) return;
    if (!valid_codepoint(code) || code < 0x20) return; // 只认可打印码点
    push_text_key(out, mods, utf8_from_codepoint(code));
}

// CSI 键盘终止符 → 键/焦点事件；识别不了的静默丢弃。
void decode_csi_key(const CsiParams& p, char final_, bool alt,
                    std::vector<Event>& out) {
    Mods mods = p.np >= 2 ? mods_from_param(p.v[1]) : Mods::none;
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
    case 'u':
        decode_kitty_key(p, alt, out);
        return;
    case '~': {
        const Key k = key_from_tilde(p.np >= 1 ? p.v[0] : 0);
        if (k != Key::none) out.push_back(key_event(k, mods, {}));
        return;
    }
    default:
        return;
    }
}

// 单个序列的扫描结果。
struct SeqResult {
    std::size_t next = 0;    ///< 消费到的位置
    bool enter_paste = false; ///< 命中 \e[200~：转入粘贴收集态
    bool wait = false;        ///< 序列不完整：留在缓冲等后续
    bool reply = false;       ///< wait 时有效：窗口内未读完的应答，不参与 Esc 超时
};

// 序列内非参数字节（CSI 与 SS3 共用）的处理方式。
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

// 不完整返回：撤回已产出的 C0 事件，缓冲留待整体重新解析。
SeqResult wait_from(std::size_t start, std::vector<Event>& out, std::size_t base) {
    out.erase(out.begin() + static_cast<std::ptrdiff_t>(base), out.end());
    return {start, false, true};
}

// 终端字符串：OSC 由 BEL 或 ST 终止，其余仅 ST；只在应答窗口打开时调用。
SeqResult decode_string(std::string_view s, std::size_t start, char intro,
                        std::vector<Event>& out) {
    const bool osc = intro == ']';
    const std::size_t cap = start + k_max_reply;
    const auto type = [&] {
        switch (intro) {
        case ']': return Event::ReplyType::osc;
        case 'P': return Event::ReplyType::dcs;
        default:  return Event::ReplyType::apc; // \e_ APC；废弃的 PM/SOS 并入
        }
    }();

    std::size_t i = start;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t body_end = 0;
        std::size_t next = 0;
        if (osc && c == 0x07) {
            body_end = i;
            next = i + 1;
        } else if (c == 0x1B) {
            if (i + 1 >= s.size()) return {start, false, true, true}; // ST 只看了一半
            if (s[i + 1] != '\\') {
                ++i; // 字符串内容里的 ESC：继续
                continue;
            }
            body_end = i;
            next = i + 2;
        } else {
            ++i;
            continue;
        }
        const std::size_t end = body_end < cap ? body_end : cap;
        push_reply(out, type, s.substr(start, end - start));
        return {next, false, false};
    }
    return {start, false, true, true}; // 不完整：留在缓冲等终止符
}

// 从 start（\e[ 之后）扫描 CSI 到终止符；窗口内私有标记序列产出应答。
SeqResult decode_csi(std::string_view s, std::size_t start, bool alt,
                     bool reply_window, std::vector<Event>& out) {
    CsiParams p;
    int cur = 0;
    bool digit = false;
    int primary = 0;
    bool have_primary = false;
    int sub = 0;
    bool sub_seen = false;
    const std::size_t n = s.size();
    const std::size_t base = out.size();
    std::size_t i = start;

    const auto push = [&] {
        if (p.np < k_max_params) {
            p.v[p.np] = have_primary ? primary : cur;
            // 只有一个 ':' 时子参数还在 cur 里，多段子参数时取第一段。
            p.sub[p.np] = have_primary ? (sub_seen ? sub : cur) : 0;
            p.sub_seen[p.np] = have_primary;
            ++p.np;
        }
        cur = 0;
        primary = 0;
        sub = 0;
        digit = false;
        have_primary = false;
        sub_seen = false;
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
            // code[:alternate] / mods[:event]：首个冒号前是主值。
            if (!have_primary) {
                primary = cur;
                have_primary = true;
            } else if (!sub_seen) {
                sub = cur;
                sub_seen = true;
            }
            cur = 0;
            digit = false;
            ++i;
            continue;
        }
        if (c == '?' || c == '>' || c == '=') {
            if (p.np == 0 && p.marker == 0 && !p.sgr && !p.intermediate &&
                !digit && !have_primary) {
                p.marker = static_cast<char>(c);
            } else {
                p.junk = true;
            }
            ++i;
            continue;
        }
        if (c == '<') {
            if (p.np == 0 && p.marker == 0 && !p.sgr && !p.intermediate &&
                !digit && !have_primary) {
                p.sgr = true;
            } else {
                p.junk = true;
            }
            ++i;
            continue;
        }
        if (c >= 0x20 && c <= 0x2F) {
            p.intermediate = true; // DECRQM 的 '$' 等
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
        if (digit || p.np > 0 || have_primary || sub_seen) push();
        ++i;
        if (p.marker != 0 && reply_window) {
            push_reply(out, Event::ReplyType::csi, s.substr(start, i - start));
            return {i, false, false};
        }
        if (p.junk || p.marker != 0 || p.intermediate) {
            return {i, false, false}; // 不可识别：完整读到终止符后整体丢弃
        }
        if (p.sgr) {
            decode_mouse(p.v, p.np, static_cast<char>(c), alt, out);
            return {i, false, false};
        }
        if (c == 'M' && p.np == 0) {
            // 旧式 X10 鼠标：\e[M 后跟 3 个原始字节，连负载一起吞掉。
            if (n - i < 3) return wait_from(start, out, base); // 负载不足：等待
            return {i + 3, false, false};
        }
        if (c == '~' && p.np >= 1) {
            if (p.v[0] == 200) return {i, true, false};  // 粘贴开始
            if (p.v[0] == 201) return {i, false, false}; // 游离的结束标记
        }
        decode_csi_key(p, static_cast<char>(c), alt, out);
        return {i, false, false};
    }
    // 不完整：私有标记应答不参与 Esc 超时，其余交超时消解。
    const bool reply = reply_window && p.marker != 0;
    out.erase(out.begin() + static_cast<std::ptrdiff_t>(base), out.end());
    return {start, false, true, reply};
}

// SS3（应用键盘模式）：\eO 后至多一段参数，单终止符。
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
                // 终端字符串：只有应答窗口打开时按应答解析；多余的 ESC 是 Alt 组合键。
                if (reply_window_ && !has_alt &&
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
