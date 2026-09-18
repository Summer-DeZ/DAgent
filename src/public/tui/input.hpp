/// @file input.hpp
/// @brief 输入层：字节解码为事件，并按模态 → 焦点 → 全局顺序路由。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tui/widget.hpp"

namespace dagent::tui {

/// @brief 命名键。
/// @note 可打印字符走 Kind::text，或 Kind::key + text（Ctrl/Alt 组合）。
enum class Key : uint8_t {
    none,
    enter, tab, backspace, escape,
    left, right, up, down,
    home, end, page_up, page_down, insert, del,
    f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
};

/// @brief 修饰键位。
enum class Mods : uint8_t {
    none = 0,
    shift = 1 << 0,
    alt = 1 << 1,
    ctrl = 1 << 2,
    super = 1 << 3,
};

constexpr Mods operator|(Mods a, Mods b) noexcept {
    return static_cast<Mods>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
constexpr Mods operator&(Mods a, Mods b) noexcept {
    return static_cast<Mods>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
constexpr Mods& operator|=(Mods& a, Mods b) noexcept { return a = a | b; }
constexpr bool any(Mods m) noexcept { return static_cast<uint8_t>(m) != 0; }

/// @brief 输入事件。
struct Event {
    enum class Kind { text, key, mouse, paste, resize, focus, reply };
    /// @brief Kind::reply 的序列来源。
    /// @note text 为序列体：csi 含 \e[ 之后到最终字节的全部内容，osc/dcs/apc
    /// 为引导符之后、ST（OSC 还可用 BEL）之前的负载。
    enum class ReplyType : uint8_t { csi, osc, dcs, apc };
    Kind kind = Kind::text;
    /// 文本内容（UTF-8）；Kind::key 携带可打印字符时为单码点，命名键时为空。
    std::string text;
    Key key = Key::none;
    Mods mods = Mods::none;
    ReplyType reply_type = ReplyType::csi; ///< 仅 Kind::reply 有效
    struct Mouse {
        int button = -1; ///< 0/1/2 = 左/中/右；4/5/6/7 = 滚轮上/下/左/右；-1 = 无键
        int col = 0;     ///< 屏幕 0 基列
        int row = 0;     ///< 屏幕 0 基行
        int x = 0;       ///< 相对命中控件的列（分发时改写）
        int y = 0;       ///< 相对命中控件的行
        bool press = false;   ///< 按下/拖拽 true，释放 false
        bool motion = false;  ///< 移动（拖拽或悬停）
        bool outside = false; ///< 点在该模态浮层之外
    } mouse;
    Size size{};               ///< Kind::resize 的新尺寸
    bool focus_gained = false; ///< Kind::focus：true 获得焦点，false 失去
};

/// @brief 增量解码器：喂入任意分块字节，产出事件。
class Decoder {
public:
    /// Esc 歧义窗口（ms）；pending_escape() 为 true 时 poll 应带此超时。
    static constexpr int k_escape_timeout_ms = 40;

    /// @brief 开关查询应答窗口；窗口内终端查询的应答产出 Kind::reply。
    void set_reply_window(bool open) noexcept;

    /// @brief 追加字节并解析，事件追加到 out。
    void feed(std::string_view bytes, std::vector<Event>& out);

    /// @brief 缓冲是否停在转义歧义窗口（true 时 poll 应带 Esc 超时）。
    bool pending_escape() const noexcept;

    /// @brief 超时消解转义歧义窗口，产出按键事件（应在 poll 超时后调用）。
    void flush_escape(std::vector<Event>& out);

private:
    /// @brief 收集括号粘贴内容；返回 true 表示仍在收集。
    bool drain_paste(std::vector<Event>& out);

    std::string buf_;               ///< 未消费字节
    bool paste_ = false;            ///< 正在收集括号粘贴
    bool reply_window_ = false;     ///< 应答窗口是否打开
    bool reply_pending_ = false;    ///< buf_ 中是窗口内未读完的应答
    std::size_t paste_scanned_ = 0; ///< 粘贴内容中已扫描过结束标记的前缀长度
};

/// @brief 事件处理器：返回 true 表示已消费，事件不再下沉。
class EventHandler {
public:
    virtual ~EventHandler() = default;
    virtual bool on_event(const Event& e) = 0;
};

/// @brief 事件路由：模态栈 → 焦点 → 全局，首个消费的处理器终止下沉。
/// @note 处理器不归路由所有，压栈/弹栈必须配对。
class EventRouter {
public:
    /// @brief 模态处理器压栈/按身份弹出。
    void push(EventHandler& h);
    void pop(EventHandler& h);

    void set_focus(EventHandler* f) noexcept { focus_ = f; }
    void set_global(EventHandler* g) noexcept { global_ = g; }

    /// @brief 下发一个事件，返回是否被消费。
    /// @note 处理器可在 on_event 内 push/pop：新压入的处理器不参与本次下发，
    /// 被弹出的处理器立即不再被调用，可随即销毁。
    bool route(const Event& e);

private:
    std::vector<EventHandler*> stack_; ///< 下发期间可能含空槽（已弹出）
    EventHandler* focus_ = nullptr;
    EventHandler* global_ = nullptr;
    int routing_ = 0;        ///< 下发嵌套深度
    bool has_holes_ = false; ///< 下发期间是否留下空槽
};

/// @brief InputBox 的按键翻译层：文本与编辑键转成模型调用。
/// @note enter、escape、带修饰的方向键不消费，提交/补全策略由上层决定。
class InputBoxHandler : public EventHandler {
public:
    explicit InputBoxHandler(InputBox& box) noexcept : box_(box) {}

    bool on_event(const Event& e) override;

private:
    InputBox& box_;
};

} // namespace dagent::tui
