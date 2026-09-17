// L6 输入层：字节解码 → 事件 → 焦点链路由（文档§九）。
//
// 解码与语义严格分离，这是避免"按键归谁管"这类冲突的结构性办法：
//   * Decoder 是纯字节状态机：不知道任何控件，不持有时钟（Esc 歧义的
//     超时判定权交给调用方的 poll 循环，见 k_escape_timeout_ms）；
//   * EventRouter 是固定的下沉顺序：处理器栈 → 焦点 → 全局兜底，
//     "某个键在某种状态下归谁"由栈的顺序回答，不需要任何条件判断；
//   * InputBoxHandler 是 L4 约定的按键翻译（widget.hpp：把事件翻译成
//     InputBox 的模型调用），提交/补全等策略仍归上层。
//
// 解码器不产生 resize：终端尺寸由渲染线程逐帧 ioctl 探测（L1），
// L7 合成 Kind::resize 事件（新尺寸填入 Event::size）塞进同一条事件流。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tui/widget.hpp"

namespace dagent::tui {

// 命名键。可打印字符不走这里 —— 走 Kind::text（未加修饰时）或
// Kind::key + text 携带字符（Ctrl/Alt 组合，文本事件无法表达修饰）。
enum class Key : uint8_t {
    none,
    enter, tab, backspace, escape,
    left, right, up, down,
    home, end, page_up, page_down, insert, del,
    f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
};

// 修饰键位。组合规则与 xterm 的 modifier 参数一致（§9.1）。
enum class Mods : uint8_t {
    none = 0,
    shift = 1 << 0,
    alt = 1 << 1,
    ctrl = 1 << 2,
};

constexpr Mods operator|(Mods a, Mods b) noexcept {
    return static_cast<Mods>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
constexpr Mods operator&(Mods a, Mods b) noexcept {
    return static_cast<Mods>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
constexpr Mods& operator|=(Mods& a, Mods b) noexcept { return a = a | b; }
constexpr bool any(Mods m) noexcept { return static_cast<uint8_t>(m) != 0; }

struct Event {
    enum class Kind { text, key, mouse, paste, resize, focus };
    Kind kind = Kind::text;
    // text/paste 的内容（UTF-8）；Kind::key 携带可打印字符时是单码点
    // （例如 Ctrl-A → mods=ctrl, text="a"），命名键时为空。
    std::string text;
    Key key = Key::none;
    Mods mods = Mods::none;
    struct Mouse {
        int button = -1; // 0/1/2 = 左/中/右；4/5/6/7 = 滚轮上/下/左/右；-1 = 无键
        int col = 0;     // 0 基列（1006 编码是 1 基，解码时已减一）
        int row = 0;     // 0 基行
        bool press = false;  // 按下/拖拽 true，释放 false
        bool motion = false; // 移动（拖拽或悬停）
    } mouse;
    // Kind::resize 的新尺寸（L7 逐帧 ioctl 探测后合成，解码器不产生）。
    Size size{};
    bool focus_gained = false; // Kind::focus：true 获得焦点（\e[I），false 失去
};

// 增量解码器：喂进任意分块的字节流，吐出事件（§9.1）。
// 内部缓冲把跨读取边界的多字节 UTF-8 与截断的转义序列拼回来；
// 无法识别的序列完整读到终止符再整体丢弃 —— 残留字节混进输入框
// 是终端程序最常见的 bug，这里在结构上排除。
//
// 同一次 feed 内连续的可打印字符合并为一个 text 事件（大段输入只触发
// 一次上层插入），因此分块不变性约束的是"拼接后的文本"，不是事件边界；
// 孤立 ESC 的歧义消解除外 —— 它本质上由时间决定。
class Decoder {
public:
    // Esc 歧义窗口：pending_escape() 为 true 时，调用方给 poll 加这么多
    // 毫秒的超时；超时仍无后续字节即调 flush_escape() 判为单独 Esc。
    static constexpr int k_escape_timeout_ms = 40;

    // 追加字节并解析，产出的事件追加到 out 尾部。
    void feed(std::string_view bytes, std::vector<Event>& out);

    // 是否停在转义歧义窗口：缓冲以 ESC 开头且不完整（孤立 ESC 串、
    // \e[ / \eO 引导符、已收部分参数的残缺 CSI）。true 时 poll 应带
    // k_escape_timeout_ms 超时。
    bool pending_escape() const noexcept;

    // 转义歧义窗口的超时消解：
    //   * 孤立 ESC 串 —— 每个 ESC 产出一个 Esc 键事件；
    //   * \e[ / \eO —— 产出 Alt-[ / Alt-O（用户按了 alt 组合键后停手）；
    //   * 已带参数的残缺序列 —— 整体丢弃，不产出事件。
    // 只应在 poll 超时（窗口内确实没有后续字节）后调用；此前调用会把
    // 序列前缀误判成按键。
    void flush_escape(std::vector<Event>& out);

private:
    // 括号粘贴：结束标记 \e[201~ 按 paste_scanned_ 增量查找，避免
    // 大粘贴内容被反复重扫。返回 true 表示仍处于粘贴收集态。
    bool drain_paste(std::vector<Event>& out);

    std::string buf_;               // 未消费字节；ground 态残留至多一个不完整单元
    bool paste_ = false;            // 正在收集括号粘贴内容
    std::size_t paste_scanned_ = 0; // 粘贴内容中已排除过结束标记的前缀长度
};

// 事件处理器：返回 true 表示已消费，事件不再下沉。
class EventHandler {
public:
    virtual ~EventHandler() = default;
    virtual bool on_event(const Event& e) = 0;
};

// 焦点链 + 处理器栈路由（§9.2）。下沉顺序固定：
//   [ 栈顶模态处理器 ]（搜索框、确认对话、滚动处理器…临时压栈）
//   [ 焦点处理器 ]（当前聚焦控件的翻译层，可空）
//   [ 全局处理器 ]（Ctrl-C / Ctrl-D / 全局快捷键，可空）
// 第一个返回 true 的消费事件。处理器不归路由所有：压栈/弹栈必须配对。
class EventRouter {
public:
    // 模态处理器压栈/按身份弹出（后进先出；弹非栈顶是使用方违约）。
    void push(EventHandler& h);
    void pop(EventHandler& h);

    void set_focus(EventHandler* f) noexcept { focus_ = f; }
    void set_global(EventHandler* g) noexcept { global_ = g; }

    // 下发一个事件，返回是否被消费。处理器可以在 on_event 里 push/pop
    // 本路由（确认框弹出自己、连同下层模态一起关闭并销毁）：
    //   * 期间压栈的处理器不参与本次下发；
    //   * 期间弹出的处理器立即不再被调用 —— 包括本次还没轮到的，
    //     所以弹出后马上销毁是安全的。
    // 下发期间 pop 只把槽位置空（下标稳定），最外层下发结束时统一压实：
    // 不复制栈，零分配。
    bool route(const Event& e);

private:
    std::vector<EventHandler*> stack_; // 下发期间可能含空槽（已弹出）
    EventHandler* focus_ = nullptr;
    EventHandler* global_ = nullptr;
    int routing_ = 0;       // 下发嵌套深度（处理器里可能再次 route）
    bool has_holes_ = false; // 下发期间是否留下了空槽
};

// InputBox 的按键翻译层（widget.hpp 约定的 L6 职责）：
//   可打印字符与粘贴 → insert（粘贴里的 \r\n 已规范化，换行不触发提交）；
//   退格/删除/方向/Home/End/Tab → 对应模型调用。
// enter、escape、带修饰的方向键一律不消费 —— 提交、补全、词间移动
// 是应用策略，由模态或全局处理器决定。
class InputBoxHandler : public EventHandler {
public:
    explicit InputBoxHandler(InputBox& box) noexcept : box_(box) {}

    bool on_event(const Event& e) override;

private:
    InputBox& box_;
};

} // namespace dagent::tui
