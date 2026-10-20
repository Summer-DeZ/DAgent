/// @file files_detail.hpp
/// @brief workspace 文件实现共用的路径、文本编码与错误处理。
#pragma once

#include "workspace/files.hpp"

namespace dagent::workspace::detail {

[[noreturn]] void fail(WorkspaceError::Kind kind, std::string_view op, const std::filesystem::path& p,
                       const std::string& detail = {});
std::filesystem::path through_symlink(const std::filesystem::path& p);
void decode_text(std::string& raw, const std::filesystem::path& p, TextFile& out);
std::string encode_text(std::string_view content, Eol eol, bool bom);
std::string encode_reference_text(std::string_view content, Eol eol, bool bom);

} // namespace dagent::workspace::detail
