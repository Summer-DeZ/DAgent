/// @file bootstrap.hpp
/// @brief 后端装配入口：读取配置、初始化日志，构造 backend 与 runtime 端口的具体实现。
///
/// dagent-backend 的入口把 assemble_backend 注入 backend::Backend；backend 只看到装配契约与抽象端口。
#pragma once

#include "backend/assembly.hpp"

namespace dagent::app {

/// @brief 实现 backend::Assembler：配置错误翻译为 backend::ConfigurationError。
backend::Assembled assemble_backend(const backend::BootstrapOptions& options);

} // namespace dagent::app
