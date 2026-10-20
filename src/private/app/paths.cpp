#include "app/paths.hpp"

#include <cstdlib>
#include <system_error>

namespace dagent::app {
namespace fs = std::filesystem;

HomePaths home_paths() {
    fs::path root;
    if (const char* value = std::getenv("DAGENT_HOME"); value != nullptr && *value != '\0') {
        std::error_code ec;
        root = fs::weakly_canonical(value, ec);
        if (ec) root = fs::absolute(value).lexically_normal();
    } else {
#ifdef DAGENT_DEV_HOME
        std::error_code ec;
        root = fs::weakly_canonical(DAGENT_DEV_HOME, ec);
        if (ec) root = fs::absolute(DAGENT_DEV_HOME).lexically_normal();
#else
        root = detail::executable_home_directory();
#endif
    }
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        throw InstallError("installation root does not exist or is not a directory: " + root.string());

    detail::probe_home_writable(root);
    return HomePaths(root);
}

} // namespace dagent::app
