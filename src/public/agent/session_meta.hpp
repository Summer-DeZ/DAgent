/// @file session_meta.hpp
/// @brief 会话身份与持久元信息的中立值类型。
#pragma once

#include <filesystem>
#include <string>

namespace dagent::agent {

struct SessionMeta {
    std::string id;
    std::filesystem::path cwd, git_root;
    std::string model;
    std::string created;
    std::string parent_id;
    std::string agent_name;
};

} // namespace dagent::agent
