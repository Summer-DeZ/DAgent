#include "storage/platform.hpp"
#include "base/platform.hpp"

namespace dagent::storage::detail {

void make_decision_file_private(const std::filesystem::path& path) {
    std::error_code error;
    base::make_file_private(path, error);
}

} // namespace dagent::storage::detail
