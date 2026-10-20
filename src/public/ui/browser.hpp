#pragma once

#include <string>

namespace dagent::ui {
// Frontend-only launcher; no shell, no inherited terminal streams.
bool open_browser(const std::string& url);
}
