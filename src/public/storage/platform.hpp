/// @file platform.hpp
/// @brief storage 内部的平台文件锁与决策库权限。
#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

namespace dagent::storage::detail {

class SessionFileLock {
public:
    SessionFileLock(const std::filesystem::path& path, std::string_view session_id);
    ~SessionFileLock();
    SessionFileLock(const SessionFileLock&) = delete;
    SessionFileLock& operator=(const SessionFileLock&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void make_decision_file_private(const std::filesystem::path& path);

} // namespace dagent::storage::detail
