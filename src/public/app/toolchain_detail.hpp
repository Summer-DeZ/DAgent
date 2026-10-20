/// @file toolchain_detail.hpp
/// @brief 托管运行时的平台标识与安装锁，仅供 app 内部使用。
#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace dagent::app::detail {

std::string runtime_platform();

class SyncLock {
public:
    explicit SyncLock(const std::filesystem::path& path);
    ~SyncLock();
    SyncLock(const SyncLock&) = delete;
    SyncLock& operator=(const SyncLock&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dagent::app::detail
