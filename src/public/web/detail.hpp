/// @file detail.hpp
/// @brief 网页字符集转换，仅供 web 内部使用。
#pragma once

#include <string>

namespace dagent::web::detail {

std::string convert_charset(std::string body, const std::string& charset, bool truncated);

} // namespace dagent::web::detail
