/// @file log.hpp
/// @brief 全局日志初始化与具名 logger（spdlog 薄约定）。日志默认只写文件：
/// 交互模式下终端归 TUI 所有，写到 stdout/stderr 的内容会把界面弄花。
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

namespace dagent::base {

/// @brief 日志选项，对应 config.json 的 "log" 段。级别会被环境变量 DAGENT_LOG 覆盖。
struct LogOptions {
    std::filesystem::path file;             ///< 入口固定为 <root>/logs/dagent-<pid>.log
    std::size_t max_file_bytes = 0;   ///< 单文件上限，写满后滚动
    std::size_t max_files = 0;              ///< 日志文件总数上限（含当前文件）
    std::string level;             ///< 全局默认级别
    bool also_stderr = false;               ///< 只在非交互模式（cli --verbose）下打开
};

/// @brief 初始化全局日志：文件 sink、级别、定时刷盘与崩溃刷盘。重复调用会先关闭上一次的 logger。
/// 级别解析失败抛 std::invalid_argument。
void init_log(const LogOptions& opt);

/// @brief 取模块具名 logger（"net"、"exec"……），所有模块共用同一组 sink。
/// init_log 之前返回 null logger，各模块在 temp 检测程序里单独使用时不会崩溃。
std::shared_ptr<spdlog::logger> logger(std::string_view module);

/// @brief 刷盘并释放所有 logger；退出或崩溃时调用。
void shutdown_log();

} // namespace dagent::base
