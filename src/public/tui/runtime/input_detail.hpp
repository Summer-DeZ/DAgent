/// @file input_detail.hpp
/// @brief 转义序列解码：序列解码（input_decode.cpp）与 Decoder（input.cpp）共用。
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "tui/runtime/input.hpp"

namespace dagent::tui::input_detail {

/// @brief s[0] 处 UTF-8 序列长度：缓冲不足且前缀合法 → 0（等更多字节），非法 → -1（丢首字节）。
int utf8_length(std::string_view s) noexcept;
Event key_event(Key k, Mods m, std::string_view ch);
/// @brief C0 控制字节 → 键事件；Ctrl-字母用 text 携带字母、mods=ctrl 表达。
Event control_event(unsigned char b, bool alt);

/// @brief 单个序列的扫描结果。
struct SeqResult {
    std::size_t next = 0;    ///< 消费到的位置
    bool enter_paste = false; ///< 命中 \e[200~：转入粘贴收集态
    bool wait = false;        ///< 序列不完整：留在缓冲等后续
    bool reply = false;       ///< wait 时有效：窗口内未读完的应答，不参与 Esc 超时
};

/// @brief 终端字符串：OSC 由 BEL 或 ST 终止，其余仅 ST。
SeqResult decode_string(std::string_view s, std::size_t start, char intro,
                        std::vector<Event>& out);
/// @brief 从 start（\e[ 之后）扫描 CSI 到终止符；窗口内私有标记序列产出应答。
SeqResult decode_csi(std::string_view s, std::size_t start, bool alt,
                     bool reply_window, std::vector<Event>& out);
/// @brief SS3（应用键盘模式）：\eO 后至多一段参数，单终止符。
SeqResult decode_ss3(std::string_view s, std::size_t start, bool alt,
                     std::vector<Event>& out);

} // namespace dagent::tui::input_detail
