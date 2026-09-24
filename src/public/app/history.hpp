/// @file history.hpp
/// @brief 装配侧的会话 ID 解析：精确 id 或唯一前缀，prefix 为空时选择最近会话。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "storage/storage.hpp"

namespace dagent::app {

/// @brief 在当前项目的会话中解析完整 id 或唯一前缀；prefix 为空时选择最近会话。
std::string resolve_session_id(const storage::Options&, const std::filesystem::path& cwd,
                               std::optional<std::string_view> prefix);

} // namespace dagent::app
