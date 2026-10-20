#include "storage/platform.hpp"

#include <sys/stat.h>

namespace dagent::storage::detail {

void make_decision_file_private(const std::filesystem::path& path) {
    ::chmod(path.c_str(), 0600);
}

} // namespace dagent::storage::detail
