/// @file ports.hpp
/// @brief 核心端口导航：业务核心定义的能力契约，由外层模块实现。
///
/// 只作包含导航；契约按业务拆在窄头文件里，不能成为装下所有外层配置的总头。
#pragma once

#include "agent/port_model.hpp"
#include "agent/port_journal.hpp"
#include "agent/port_store.hpp"
#include "agent/port_interaction.hpp"
#include "agent/port_tool.hpp"
#include "agent/port_delegation.hpp"
