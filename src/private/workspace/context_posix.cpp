#include "workspace/context.hpp"

#include <format>
#include <cstdlib>
#include <sys/utsname.h>

namespace dagent::workspace::detail {

std::string os_description() {
    struct utsname name {};
    return ::uname(&name) == 0 ? std::format("{} {}", name.sysname, name.release) : "unknown";
}

std::string shell_description() {
    if (const char* shell = std::getenv("SHELL"); shell != nullptr && *shell != '\0') return shell;
    return "sh";
}

} // namespace dagent::workspace::detail
