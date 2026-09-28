/// @file detail.hpp
/// @brief tools 内部跨实现文件共享的细节，不属于对外接口；外部代码不要 include。
#pragma once

#include <cstdint>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"
#include "tools/tools.hpp"
#include "workspace/files.hpp"

namespace dagent::tools::detail {

using base::parse_arguments;
using base::require_string;
using base::get_string;
using base::get_int;
using base::get_bool;

// ---------------------------------------------------------------- 结果与路径

Result error_result(std::string text, agent::View display = std::monostate{});

/// @brief 开头的 ~/ 展开成 $HOME，其余原样。
std::string expand_home(std::string_view raw);

/// @brief 所有工具的路径参数统一走这里：~/ 展开 + workspace::resolve（相对 ctx.root()）。
workspace::Resolved resolve_arg(const Context& ctx, std::string_view raw);

/// @brief 界面显示用的路径：工作区内显示相对路径，工作区外显示绝对路径。
std::string display_path(const Context& ctx, const workspace::Resolved& resolved);

/// @brief grep / glob 结果路径的前缀：dir 相对 base 的形式（"sub/"），dir == base 时空串。
/// workspace::grep/files 返回的路径相对查询根，拼上它才是模型可用的、相对工作区根的路径。
std::string relative_prefix(const std::filesystem::path& dir, const std::filesystem::path& base);

// ---------------------------------------------------------------- 文本组装

/// @brief 把 CRLF / 单独 CR 统一成 LF（模型给的 old_string、content 都先过这里）。
std::string to_lf(std::string_view text);

/// @brief 按 '\n' 拆行，不含换行本身；空内容返回空；结尾换行不产生空行。
std::vector<std::string_view> split_lines(std::string_view content);

/// @brief 行数：split_lines 的 size；结尾没有换行时最后一段也算一行。
std::size_t count_lines(std::string_view content);

/// @brief 单行超过 max_bytes 时在 UTF-8 字符边界截断并追加 "…"。
std::string fit_line(std::string_view line, std::size_t max_bytes);

/// @brief read / edit 输出里的行前缀：行号 + Tab。
std::string line_prefix(std::size_t number);

/// @brief 找出 old 在 content 里所有不重叠的出现位置。
std::vector<std::size_t> find_all(std::string_view content, std::string_view needle);

/// @brief position 前的换行数 + 1：1 开始的行号。
std::size_t line_at(std::string_view content, std::size_t position);

// ---------------------------------------------------------------- 各工具的工厂（registry.cpp 用）

std::unique_ptr<Tool> make_read_tool();
std::unique_ptr<Tool> make_write_tool();
std::unique_ptr<Tool> make_edit_tool();
std::unique_ptr<Tool> make_bash_tool();
std::unique_ptr<Tool> make_grep_tool();
std::unique_ptr<Tool> make_glob_tool();
std::unique_ptr<Tool> make_mcp_tool(std::shared_ptr<mcp::Client> client, const mcp::Tool& tool);

} // namespace dagent::tools::detail
