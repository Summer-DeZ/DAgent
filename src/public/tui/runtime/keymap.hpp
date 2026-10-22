/// @file keymap.hpp
/// @brief 快捷键与命令层：命令表项与按键串绑定。
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tui/runtime/input.hpp"

namespace dagent::tui {

/// @brief 命令表项：id 供绑定引用，title/category 供命令面板显示。
struct Command {
    std::string id;       ///< 如 "session.new"
    std::string title;
    std::string category;
    std::function<void()> run;
    std::function<bool()> enabled; ///< 空 = 始终可用
};

/// @brief 快捷键与命令层：绑定串（空格分隔，如 "ctrl+p"、"<leader> n"）到命令。
/// @note 必须先于 Runtime 销毁。
class Keymap : public EventHandler {
public:
    Keymap() = default;
    Keymap(const Keymap&) = delete;
    Keymap& operator=(const Keymap&) = delete;

    /// @brief 添加命令；重复 id 覆盖。
    void add(Command c);
    /// @brief 注册绑定；按键串非法或命令 id 不存在时返回 false。
    bool bind(std::string_view keys, std::string_view command_id);
    const std::vector<Command>& commands() const noexcept { return commands_; }

    bool on_event(const Event& e) override;

private:
    struct KeyPress {
        Key key = Key::none;
        char32_t ch = 0; ///< 可打印码点；与 key 互斥
        Mods mods = Mods::none;
        bool operator==(const KeyPress&) const noexcept = default;
    };
    struct Binding {
        KeyPress key;
        size_t command = 0; ///< commands_ 下标
    };

    static std::optional<KeyPress> press_from(const Event& e);
    static bool parse_key(std::string_view text, Mods mods, KeyPress& out);
    static bool parse_binding(std::string_view text, KeyPress& out);

    void execute(size_t command_index);

    std::vector<Command> commands_;
    std::vector<Binding> bindings_;

};

} // namespace dagent::tui
