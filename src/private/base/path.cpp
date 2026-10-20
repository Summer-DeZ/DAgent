#include "base/path.hpp"

#include <system_error>

namespace dagent::base {

std::filesystem::path canonical_or_absolute_or_lexical(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, ec);
    if (!ec) return result;
    ec.clear();
    result = std::filesystem::absolute(path, ec);
    return ec ? path.lexically_normal() : result.lexically_normal();
}

} // namespace dagent::base
