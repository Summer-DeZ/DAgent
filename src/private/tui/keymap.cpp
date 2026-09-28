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
    KeyPress key;
    if (!parse_binding(keys, key)) return false;
    size_t index = 0;
    while (index < commands_.size() && commands_[index].id != command_id) ++index;
    if (index == commands_.size()) return false;
    for (Binding& binding : bindings_) {
        if (binding.key == key) { binding.command = index; return true; }
    }
    bindings_.push_back({key, index});
    return true;
}

bool Keymap::on_event(const Event& event) {
    const auto key = press_from(event);
    if (!key) return false;
    for (const Binding& binding : bindings_) {
        if (binding.key == *key) { execute(binding.command); return true; }
    }
    return false;
}

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

bool Keymap::parse_binding(std::string_view text, KeyPress& out) {
    if (text.empty()) return false;
    Mods mods = Mods::none;
    size_t i = 0;
    for (;;) {
        const size_t plus = text.find('+', i);
        const std::string_view part = plus == std::string_view::npos
                                          ? text.substr(i)
                                          : text.substr(i, plus - i);
        if (part.empty()) return false;
        if (plus == std::string_view::npos) {
            return parse_key(part, mods, out);
        }
        if (!parse_modifier(part, mods)) return false;
        i = plus + 1;
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
