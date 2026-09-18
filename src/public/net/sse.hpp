/// @file sse.hpp
/// @brief Server-Sent Events 增量解析器（按 WHATWG 规范），配合 HttpClient::stream 使用。
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace dagent::net {

struct SseEvent {
    std::string event; ///< "event:" 字段；未出现时为空（规范上等同于 "message"）
    std::string data;  ///< 多个 "data:" 行以 '\n' 连接
    std::string id;    ///< 最近一次 "id:" 字段
};

/// @brief 喂入任意切分的字节流，遇空行派发一个事件；注释行（": keep-alive" 之类）直接跳过。
/// 流结束时未以空行收尾的残余事件按规范丢弃。
class SseParser {
public:
    using Handler = std::function<void(const SseEvent&)>;

    void feed(std::string_view chunk, const Handler& on_event);

private:
    void line(std::string_view ln, const Handler& on_event);

    std::string buf_;         ///< 尚未遇到行尾的半行
    SseEvent cur_;
    bool has_data_ = false;   ///< 当前事件是否出现过 data 字段（空 data 也算）
    bool pending_cr_ = false; ///< 上一段以 '\r' 结尾，下一段开头的 '\n' 属于同一个行尾
    bool bom_checked_ = false;
};

} // namespace dagent::net
