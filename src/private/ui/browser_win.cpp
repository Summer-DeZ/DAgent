#include "ui/browser.hpp"

#include <windows.h>
#include <shellapi.h>

namespace dagent::ui {

bool open_browser(const std::string& url) {
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(),
                                         static_cast<int>(url.size()), nullptr, 0);
    if (size == 0) return false;
    std::wstring native(static_cast<std::size_t>(size), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), static_cast<int>(url.size()),
                              native.data(), size) == 0)
        return false;
    return reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", native.c_str(), nullptr, nullptr,
                                                    SW_SHOWNORMAL)) > 32;
}

} // namespace dagent::ui
