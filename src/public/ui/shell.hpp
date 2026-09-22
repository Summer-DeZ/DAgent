#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include "agent/headless.hpp"
#include "agent/model_input.hpp"
#include "agent/port_model.hpp"
#include "agent/public_model.hpp"

namespace dagent::ui {

/// @brief 一次模型选择的结果：公开描述 + 已配置客户端；不含凭据。
struct ModelSelection {
    agent::PublicModel provider;
    std::shared_ptr<agent::ModelSession> session;
};

struct InteractiveOptions {
    std::map<std::string, agent::PublicModel> models;
    std::vector<agent::ProviderKindInfo> provider_kinds;
    std::function<ModelSelection(const std::string&)> resolve_model;
    std::function<ModelSelection(agent::ModelInput)> add_model;
    std::string initial_prompt;
    std::optional<std::string> resume_id;
    bool continue_last = false;
    std::optional<std::filesystem::path> theme_file;
};

/// 创建/恢复发生在进入全屏之前；Interrupts 把进程信号桥接到界面退出。
int run_interactive(agent::Setup, const InteractiveOptions&, agent::Interrupts&);
} // namespace dagent::ui
