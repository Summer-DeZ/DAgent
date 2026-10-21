#include "web/detail.hpp"

#include <utility>

namespace dagent::web::detail {

std::string convert_charset(std::string body, const std::string& charset, bool truncated) {
    return convert_charset_iconv(std::move(body), charset, truncated);
}

} // namespace dagent::web::detail
