/// @file process.hpp
/// @brief 一次性命令执行：带超时、可取消、流式拿到输出、输出有上限，结束时连同所有子孙进程一起清理。
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/text.hpp"

namespace dagent::exec {

struct Prepared;

struct Environment {
    std::filesystem::path shell;
    std::vector<std::pair<std::string, std::string>> variables;
    std::vector<std::filesystem::path> readable;
};

/// @brief 执行选项，对应 config/dagent.json 的 "process" 段。
struct Options {
    std::vector<std::pair<std::string, std::string>> environment;
    std::chrono::milliseconds default_timeout{300000};  ///< 未指定 timeout 时用；为 0 表示不限
    std::size_t max_output_bytes = 256 << 10;           ///< 每一路输出保留的上限，超出后保留头尾
    std::chrono::milliseconds kill_grace{2000};         ///< SIGTERM 之后等多久再 SIGKILL
    std::chrono::milliseconds drain_after_exit{200};    ///< 主进程退出后最多再读多久管道
    /// 名字匹配这些通配符（不区分大小写）的继承环境变量不传给子进程；env_set 显式传入的不受影响。
    std::vector<std::string> env_deny{"*KEY*", "*TOKEN*", "*SECRET*", "*PASSWORD*"};
};

struct Command {
    std::vector<std::string> argv;          ///< argv[0] 按 PATH 查找；不经过 shell
    std::filesystem::path cwd;              ///< 为空时继承当前工作目录
    std::vector<std::pair<std::string, std::string>> env_set;
    std::vector<std::string> env_unset;
    std::optional<std::string> stdin_data;  ///< 为空时接 /dev/null
    std::optional<std::chrono::milliseconds> timeout;  ///< 为空时用 Options::default_timeout
    bool merge_stderr = false;              ///< true 时 stderr 写进 stdout 管道，保持交错顺序
    bool inherit_env = true;                ///< false 时只使用默认项与 env_set，适合受限命令
    const Prepared* sandbox = nullptr;      ///< 不为空时在子进程里应用沙箱（exec/sandbox.hpp）
};

enum class Stream { out, err };

struct Result {
    std::optional<int> exit_code;   ///< 正常退出时有值
    std::optional<int> signal;      ///< 被信号杀死时有值
    bool timed_out = false;
    base::Truncated out, err;       ///< 各自按 Options::max_output_bytes 截断
    std::chrono::milliseconds elapsed;
};

/// @brief 传输/系统层面的失败。退出码非 0、被信号杀死都属于正常结果，不抛异常。
class ExecError : public std::runtime_error {
public:
    enum class Kind {
        spawn_failed,  ///< 命令不存在、没有执行权限、fork/exec/建管道失败
        cancelled,     ///< stop_token 请求停止
        sandbox,       ///< 沙箱准备或应用失败
    };

    ExecError(Kind kind, const std::string& what) : std::runtime_error(what), kind_(kind) {}
    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

/// @brief 阻塞执行一条命令。on_output 在调用线程上按数据到达顺序触发，拿到的是未截断的数据。
/// 超时或被信号终止都返回结果并置位；取消抛 ExecError{cancelled}。抛出时进程组已经清理完。
Result run(const Command& cmd, const Options& opt = {},
           const std::function<void(Stream, std::string_view)>& on_output = {},
           std::stop_token stop = {});

/// @brief 按 PATH 查找可执行文件；name 含 '/' 时直接检查该路径。
std::optional<std::filesystem::path> which(std::string_view name);

/// @brief 用单引号把参数包成可安全拼进 bash 命令的字面量。
std::string shell_quote(std::string_view s);

} // namespace dagent::exec
