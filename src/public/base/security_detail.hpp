/// @file security_detail.hpp
/// @brief Windows 平台实现内部共用的私有 ACL 编解码；不暴露 Windows 头、句柄或结构类型。
#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace dagent::base::detail {

/// 自相对安全描述符：当前用户所有，三个受信任主体有完全控制，禁止继承父 ACL。
std::vector<std::byte> private_security_descriptor();
/// 仅接受受保护、无其他获准主体的普通 allow ACE；检查当前用户的读写（目录含遍历）权限。
/// 凭据要求当前用户所有；配置可由上述三个受信任主体之一所有。
bool is_private_security_descriptor(std::span<const std::byte> descriptor,
                                    bool directory, bool require_current_owner);

} // namespace dagent::base::detail
