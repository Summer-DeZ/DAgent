#pragma once

#include <string>
#include <string_view>

namespace dagent::app {

std::string sha256(std::string_view data);

} // namespace dagent::app
