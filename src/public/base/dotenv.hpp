/// @file dotenv.hpp
/// @brief `.env` 文件解析与密钥查找。文件里的值只保存在 Secrets 表里，不写入进程环境。
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dagent::base {

/// @brief 解析 .env 文本，按出现顺序返回键值对。规则见 .env.example：
/// `KEY=VALUE`；`#` 开头是注释；可以带 `export ` 前缀；值可用单引号或双引号包裹，
/// 只有双引号里的 `\n` 做转义；不带引号的值在「空白 + #」处结束，引号内的 `#` 属于值；
/// 去掉首尾空白。
std::vector<std::pair<std::string, std::string>> parse_dotenv(std::string_view text);

/// @brief 密钥表。不调用 setenv：写进进程环境后，模型通过 bash 执行的每条命令都会继承密钥。
class Secrets {
public:
    /// @brief 依次读取这些文件，文件不存在就跳过；同一键先读到的优先。
    static Secrets load(std::span<const std::filesystem::path> files);

    /// @brief 先查进程环境变量，再查文件内容；都没有返回 nullopt。
    std::optional<std::string> get(std::string_view name) const;

private:
    std::unordered_map<std::string, std::string> values_;
};

} // namespace dagent::base
