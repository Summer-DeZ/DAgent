#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include "agent/headless.hpp"

namespace dagent::ui {
struct InteractiveOptions {
    std::string initial_prompt;
    std::optional<std::string> resume_id;
    bool continue_last = false;
    std::optional<std::filesystem::path> theme_file;
};

/// 创建/恢复发生在进入全屏之前；Interrupts 把进程信号桥接到界面退出。
int run_interactive(agent::Setup, const InteractiveOptions&, agent::Interrupts&);
} // namespace dagent::ui
