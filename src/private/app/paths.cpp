#include "app/paths.hpp"
#include "base/path.hpp"
#include "base/platform.hpp"

#include <system_error>

namespace dagent::app {
namespace fs = std::filesystem;

HomePaths home_paths() {
    fs::path root;
    if (const auto value = base::environment_variable("DAGENT_HOME"); value && !value->empty()) {
        std::error_code ec;
        root = fs::weakly_canonical(base::path_from_utf8(*value), ec);
        if (ec) root = fs::absolute(base::path_from_utf8(*value)).lexically_normal();
    } else {
#ifdef DAGENT_DEV_HOME
        std::error_code ec;
        root = fs::weakly_canonical(base::path_from_utf8(DAGENT_DEV_HOME), ec);
        if (ec) root = fs::absolute(base::path_from_utf8(DAGENT_DEV_HOME)).lexically_normal();
#else
        root = detail::executable_home_directory();
#endif
    }
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        throw InstallError("installation root does not exist or is not a directory: " + base::path_to_utf8(root));

    detail::probe_home_writable(root);
    return HomePaths(root);
}

} // namespace dagent::app
