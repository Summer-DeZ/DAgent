#include "app/home.hpp"

#include "app/hash.hpp"

namespace dagent::app {

std::filesystem::path HomePaths::project_memory(const std::filesystem::path& project_root) const {
    const auto canonical = std::filesystem::weakly_canonical(project_root);
    const auto key = canonical.filename().string() + "-" + sha256(canonical.generic_string()).substr(0, 8);
    return memory / "projects" / key;
}

} // namespace dagent::app
