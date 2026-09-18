#include "net/sse.hpp"

namespace dagent::net {

void SseParser::feed(std::string_view chunk, const Handler& on_event) {
    buf_.append(chunk);
    if (!bom_checked_) {
        constexpr std::string_view bom = "\xEF\xBB\xBF";
        if (buf_.size() < bom.size() && bom.starts_with(buf_)) return; // BOM 可能被切在两段之间
        if (buf_.starts_with(bom)) buf_.erase(0, bom.size());
        bom_checked_ = true;
    }

    std::size_t pos = 0;
    if (pending_cr_ && !buf_.empty()) {
        if (buf_[0] == '\n') pos = 1;
        pending_cr_ = false;
    }
    for (;;) {
        const std::size_t end = buf_.find_first_of("\r\n", pos);
        if (end == std::string::npos) break;
        line(std::string_view(buf_).substr(pos, end - pos), on_event);
        pos = end + 1;
        if (buf_[end] == '\r') {
            if (pos == buf_.size()) pending_cr_ = true;
            else if (buf_[pos] == '\n') ++pos;
        }
    }
    buf_.erase(0, pos);
}

void SseParser::line(std::string_view ln, const Handler& on_event) {
    if (ln.empty()) {
        if (has_data_) on_event(cur_);
        cur_.event.clear();
        cur_.data.clear();
        has_data_ = false; // id 按规范跨事件保留
        return;
    }
    if (ln.front() == ':') return;

    const std::size_t colon = ln.find(':');
    const std::string_view field = ln.substr(0, colon);
    std::string_view value = colon == std::string_view::npos ? std::string_view{} : ln.substr(colon + 1);
    if (value.starts_with(' ')) value.remove_prefix(1);

    if (field == "data") {
        if (has_data_) cur_.data += '\n';
        cur_.data.append(value);
        has_data_ = true;
    } else if (field == "event") {
        cur_.event.assign(value);
    } else if (field == "id") {
        if (value.find('\0') == std::string_view::npos) cur_.id.assign(value);
    }
}

} // namespace dagent::net
