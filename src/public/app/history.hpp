/// @file history.hpp
/// @brief 装配侧的只读历史投影与会话 ID 解析。
///
/// 记录编解码在 agent/record_codec.hpp；恢复在 agent/recovery.hpp；存储实现在 storage。
/// 显示投影按旧实时事件形状产出，R11 换成协议历史条目后仍由后端适配。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/events.hpp"
#include "agent/port_store.hpp"
#include "storage/storage.hpp"

namespace dagent::app {

/// @brief 只读历史投影：按 seq 解码记录并生成旧实时事件序列，供旧界面回放。
/// 不恢复可写 Session、不写库、不构造模型或 MCP。
std::vector<agent::Event> project_history(const storage::Options&, std::string_view id);

/// @brief 在当前项目的会话中解析完整 id 或唯一前缀；prefix 为空时选择最近会话。
std::string resolve_session_id(const storage::Options&, const std::filesystem::path& cwd,
                               std::optional<std::string_view> prefix);

} // namespace dagent::app
