/// @file bootstrap.hpp
/// @brief 后端装配入口：读取配置、初始化日志，构造 runtime 端口的具体实现。
///
/// dagent-backend 的入口把 assemble_backend 注入 backend::Backend；backend 只看到 runtime 端口。
#pragma once

#include "runtime/factory.hpp"

namespace dagent::app {

/// @brief 实现 runtime::Assembler：配置错误翻译为 runtime::ConfigurationError。
runtime::Assembled assemble_backend(const runtime::BootstrapOptions& options);

} // namespace dagent::app
