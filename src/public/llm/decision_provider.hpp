/// @file decision_provider.hpp
/// @brief 决策协议配置、适配器目录与 DecisionModel 工厂。
///
/// 第一版只注册 systemone 并实现 SystemOneDecisionModel；协议差异由适配器承担，
/// 审批路由与既定复审时机不随模型更换改变。
#pragma once

#include <chrono>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "agent/decision_types.hpp"
#include "agent/port/decision.hpp"
#include "net/http.hpp"

namespace dagent::llm {

/// @brief models.json 中 decision_models 的一个条目。
struct DecisionProviderConfig {
    std::map<std::string, std::string> headers;
    std::string name;           ///< 配置名，仅用于选择与展示
    std::string kind;           ///< 协议 kind，如 systemone
    std::string endpoint;       ///< 完整服务地址（System One 不再拼接 /v1）
    std::string model;          ///< 模型标识，如 d3
    std::string model_revision; ///< 部署方维护的权重/量化版本
};

struct DecisionCapabilities {
    bool boolean_probability = false;
    bool choice_probabilities = false;
};

struct DecisionProviderInfo {
    std::string_view kind;
    std::string_view adapter_version;
    DecisionCapabilities capabilities;
};

/// @brief 已注册的决策协议适配器（静态目录，不加载插件）。
std::vector<DecisionProviderInfo> decision_providers();
const DecisionProviderInfo* find_decision_provider(std::string_view kind);

/// @brief 按配置创建决策模型；未知 kind 抛 std::invalid_argument。
std::shared_ptr<agent::DecisionModel> make_decision_model(const DecisionProviderConfig&,
                                                          std::chrono::milliseconds timeout,
                                                          net::HttpOptions http = {});

} // namespace dagent::llm
