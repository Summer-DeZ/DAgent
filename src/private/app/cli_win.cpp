#include "app/cli.hpp"

#include <windows.h>

namespace dagent::app::detail {

std::string read_stdin_all() {
    std::string data;
    char buffer[8192];
    const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD size = 0;
    while (::ReadFile(input, buffer, static_cast<DWORD>(sizeof(buffer)), &size, nullptr) && size != 0)
        data.append(buffer, size);
    return data;
}

} // namespace dagent::app::detail
