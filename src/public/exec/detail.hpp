/// @file detail.hpp
/// @brief exec 内部跨实现文件共享的细节，不属于对外接口；外部代码不要 include。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "exec/process.hpp"

namespace dagent::exec::detail {

/// @brief 构造子进程环境：继承并按 env_deny 过滤，注入默认项、Options::environment，
/// 再应用 env_unset，最后叠加 env_set。
/// 返回值是 "名字=值" 的列表，顺序稳定。
std::vector<std::string> make_environment(const Command& cmd, const Options& opt);

/// @brief 在 "名字=值" 列表里取某个变量的值；没有时返回空。
std::string_view find_env(const std::vector<std::string>& env, std::string_view name);

/// @brief 解析可执行文件，返回绝对路径；找不到或不可执行时返回空路径。
/// POSIX 下 name 带目录时直接检查该路径，相对路径相对 cwd（为空时相对当前目录）；
/// 否则在 environment 的 PATH 里查找，不回退到宿主环境。
std::filesystem::path resolve_program(std::string_view name, const std::filesystem::path& cwd,
                                      const std::vector<std::string>& environment);

/// @brief 本进程忽略 SIGPIPE（只做一次，且只在它仍是默认处理时）：子进程提前退出后再写它的
/// stdin 管道，应当得到 EPIPE，而不是整个 agent 被信号杀死。
void ignore_sigpipe();

/// @brief 当前环境条目的独立快照，保留顺序与重复项；过滤与覆盖由 make_environment 完成。
std::vector<std::string> inherited_environment();
/// @brief 平台的环境变量名相等规则；POSIX 区分大小写，与 deny 通配符匹配独立。
/// Windows 转为 UTF-16 后按 ordinal 忽略大小写比较，转码失败可抛异常。
bool environment_names_equal(std::string_view left, std::string_view right);
bool environment_name_matches(std::string_view name, const std::string& pattern);
bool regular_executable(const std::filesystem::path& path);

} // namespace dagent::exec::detail
