#include "app/cli.hpp"

#include <cerrno>
#include <unistd.h>

namespace dagent::app::detail {

std::string read_stdin_all() {
    std::string data;
    char buffer[8192];
    for (;;) {
        const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof buffer);
        if (n > 0) {
            data.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    return data;
}

} // namespace dagent::app::detail
