#include "workspace/context.hpp"

#include <format>
#include <sys/utsname.h>

namespace dagent::workspace::detail {

std::string os_description() {
    struct utsname name {};
    return ::uname(&name) == 0 ? std::format("{} {}", name.sysname, name.release) : "unknown";
}

} // namespace dagent::workspace::detail
