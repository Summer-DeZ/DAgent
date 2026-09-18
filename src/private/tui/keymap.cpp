#include "tui/runtime.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/grapheme.hpp"

namespace dagent::tui {

namespace {

constexpr size_t k_none = static_cast<size_t>(-1);

std::optional<char32_t> single_codepoint(std::string_view text) {
    if (text.empty()) return std::nullopt;
    const char32_t cp = unicode::decode_utf8(text);
    if (!text.empty()) return std::nullopt; // 多码点文本串：不是一次按键
    return cp;
}

// 大写字母归一为小写基键 + shift（按键与绑定串两侧都经此归一）。
void normalize_letter(char32_t& cp, Mods& mods) noexcept {
    if (cp >= 'A' && cp <= 'Z') {
        cp += 32;
        mods = mods | Mods::shift;
    }
}

bool parse_modifier(std::string_view s, Mods& mods) noexcept {
    if (s == "ctrl" || s == "control") {
        mods = mods | Mods::ctrl;
    } else if (s == "alt") {
        mods = mods | Mods::alt;
    } else if (s == "shift") {
        mods = mods | Mods::shift;
    } else if (s == "super") {
        mods = mods | Mods::super;
    } else {
        return false;
    }
    return true;
}

} // namespace

Keymap::Keymap(Runtime& rt) noexcept : rt_(rt) {}

Keymap::~Keymap() {
    if (timeout_id_ != 0) rt_.cancel(timeout_id_);
    if (modal_active_) rt_.pop_modal(*this);
}

void Keymap::add(Command c) {
    for (Command& existing : commands_) {
        if (existing.id == c.id) {
            existing = std::move(c);
            return;
        }
    }
    commands_.push_back(std::move(c));
}

bool Keymap::bind(std::string_view keys, std::string_view command_id) {
    std::vector<Token> sequence;
    if (!parse_binding(keys, sequence)) return false;
    size_t index = 0;
    for (; index < commands_.size(); ++index) {
        if (commands_[index].id == command_id) break;
    }
    if (index == commands_.size()) return false; // 绑定到不存在的命令
    for (Binding& b : bindings_) {
        if (b.keys == sequence) {
            b.command = index; // 冲突：后绑定覆盖先绑定
            return true;
        }
    }
    bindings_.push_back({std::move(sequence), index});
    return true;
}

void Keymap::set_leader(std::string_view key, std::chrono::milliseconds timeout) {
    timeout_ = timeout;
    Token token;
    if (key.find(' ') != std::string_view::npos || !parse_token(key, token) ||
        token.leader) {
        leader_.reset();
        return;
    }
    leader_ = token.key;
}

bool Keymap::on_event(const Event& e) {
    const std::optional<KeyPress> press = press_from(e);
    if (!press) return false;

    const auto matches = [this](const Token& t, const KeyPress& k) {
        return t.leader ? (leader_.has_value() && *leader_ == k) : (t.key == k);
    };

    // 序列途中：先看能否精确完成，再看是否仍是某条绑定的前缀。
    if (!pending_.empty()) {
        pending_.push_back(*press);
        size_t exact = k_none;
        bool prefix = false;
        for (size_t i = 0; i < bindings_.size(); ++i) {
            const Binding& b = bindings_[i];
            if (b.keys.size() < pending_.size()) continue;
            bool ok = true;
            for (size_t k = 0; k < pending_.size(); ++k) {
                if (!matches(b.keys[k], pending_[k])) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            if (b.keys.size() == pending_.size()) {
                exact = i;
            } else {
                prefix = true;
            }
        }
        if (exact != k_none) {
            const size_t command = bindings_[exact].command;
            reset_sequence(); // 执行前先重置序列
            execute(command);
            return true;
        }
        if (prefix) {
            arm_timeout();
            return true;
        }
        reset_sequence(); // 不匹配：按键照常沿栈下沉
        return false;
    }

    // 空闲态：单键精确匹配优先；否则首键命中更长序列 → 压栈等待。
    size_t exact = k_none;
    bool prefix = false;
    for (size_t i = 0; i < bindings_.size(); ++i) {
        const Binding& b = bindings_[i];
        if (b.keys.empty() || !matches(b.keys[0], *press)) continue;
        if (b.keys.size() == 1) {
            exact = i;
        } else {
            prefix = true;
        }
    }
    if (exact != k_none) {
        execute(bindings_[exact].command);
        return true;
    }
    if (prefix) {
        pending_.push_back(*press);
        if (!modal_active_) {
            rt_.push_modal(*this);
            modal_active_ = true;
        }
        arm_timeout();
        return true;
    }
    return false;
}

// ---- 内部 ----

std::optional<Keymap::KeyPress> Keymap::press_from(const Event& e) {
    if (e.kind == Event::Kind::text) {
        const std::optional<char32_t> cp = single_codepoint(e.text);
        if (!cp) return std::nullopt;
        KeyPress k;
        k.ch = *cp;
        normalize_letter(k.ch, k.mods);
        return k;
    }
    if (e.kind != Event::Kind::key) return std::nullopt;
    KeyPress k;
    k.key = e.key;
    k.mods = e.mods;
    if (e.key == Key::none) {
        const std::optional<char32_t> cp = single_codepoint(e.text);
        if (!cp) return std::nullopt; // Ctrl/Alt + 可打印字符
        k.ch = *cp;
        normalize_letter(k.ch, k.mods);
    }
    return k;
}

bool Keymap::parse_key(std::string_view text, Mods mods, KeyPress& out) {
    out = KeyPress{};
    out.mods = mods;
    if (text.size() == 1) {
        const auto c = static_cast<unsigned char>(text[0]);
        if (c >= 0x21 && c <= 0x7E) {
            out.ch = c;
            normalize_letter(out.ch, out.mods);
            return true;
        }
        return false;
    }
    struct Named {
        std::string_view name;
        Key key;
    };
    static constexpr Named k_named[] = {
        {"enter", Key::enter},       {"return", Key::enter},
        {"tab", Key::tab},           {"backspace", Key::backspace},
        {"escape", Key::escape},     {"esc", Key::escape},
        {"left", Key::left},         {"right", Key::right},
        {"up", Key::up},             {"down", Key::down},
        {"home", Key::home},         {"end", Key::end},
        {"pageup", Key::page_up},    {"page_up", Key::page_up},
        {"pgup", Key::page_up},      {"pagedown", Key::page_down},
        {"page_down", Key::page_down}, {"pgdn", Key::page_down},
        {"insert", Key::insert},     {"ins", Key::insert},
        {"delete", Key::del},        {"del", Key::del},
    };
    for (const Named& n : k_named) {
        if (text == n.name) {
            out.key = n.key;
            return true;
        }
    }
    if (text == "space") {
        out.ch = U' ';
        return true;
    }
    static constexpr Key k_fkeys[] = {
        Key::f1,  Key::f2,  Key::f3,  Key::f4,  Key::f5,  Key::f6,
        Key::f7,  Key::f8,  Key::f9,  Key::f10, Key::f11, Key::f12,
    };
    if (!text.empty() && text[0] == 'f') {
        int n = 0;
        for (size_t i = 1; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') return false;
            n = n * 10 + (text[i] - '0');
        }
        if (n >= 1 && n <= 12) {
            out.key = k_fkeys[n - 1];
            return true;
        }
    }
    return false;
}

bool Keymap::parse_token(std::string_view text, Token& out) {
    out = Token{};
    if (text.empty()) return false;
    if (text == "<leader>") {
        out.leader = true;
        return true;
    }
    Mods mods = Mods::none;
    size_t i = 0;
    for (;;) {
        const size_t plus = text.find('+', i);
        const std::string_view part = plus == std::string_view::npos
                                          ? text.substr(i)
                                          : text.substr(i, plus - i);
        if (part.empty()) return false;
        if (plus == std::string_view::npos) {
            return parse_key(part, mods, out.key);
        }
        if (!parse_modifier(part, mods)) return false;
        i = plus + 1;
    }
}

bool Keymap::parse_binding(std::string_view text, std::vector<Token>& out) {
    out.clear();
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') ++i;
        if (i >= text.size()) break;
        const size_t end = text.find(' ', i);
        const std::string_view part = end == std::string_view::npos
                                          ? text.substr(i)
                                          : text.substr(i, end - i);
        Token token;
        if (!parse_token(part, token)) return false;
        out.push_back(std::move(token));
        if (end == std::string_view::npos) break;
        i = end;
    }
    return !out.empty();
}

void Keymap::arm_timeout() {
    if (timeout_id_ != 0) rt_.cancel(timeout_id_);
    timeout_id_ = rt_.after(timeout_, [this] {
        timeout_id_ = 0;
        reset_sequence();
    });
}

void Keymap::reset_sequence() {
    pending_.clear();
    if (timeout_id_ != 0) {
        rt_.cancel(timeout_id_);
        timeout_id_ = 0;
    }
    if (modal_active_) {
        rt_.pop_modal(*this);
        modal_active_ = false;
    }
}

void Keymap::execute(size_t command_index) {
    if (command_index >= commands_.size()) return;
    // 回调可能增删命令/绑定：先拷出再调用。
    const std::function<bool()> enabled = commands_[command_index].enabled;
    if (enabled && !enabled()) return; // 禁用：按键已消费，但不执行
    const std::function<void()> run = commands_[command_index].run;
    if (run) run();
}

} // namespace dagent::tui
