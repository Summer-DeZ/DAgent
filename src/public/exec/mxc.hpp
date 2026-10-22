/// @file mxc.hpp
/// @brief MXC 后端执行：进程内加载 libmxc_ffi，把中立 Policy 编译为 MXC 1.0.0 策略，
/// 受限命令经 `mxc_spawn_json` 启动并在本进程里读流、等待与终止。
///
/// 网络只有两种姿态：没有闸门时省略 `network`，命令完全无网络（Linux 仅 `--unshare-net`，不启动
/// slirp4netns）；有闸门时启用代理模式，MXC 保证容器只能连到本执行独占的回环代理（exec/proxy.hpp），
/// 目的地由闸门按 host:port 判定。Linux 后端是 bubblewrap，Windows 是 ProcessContainer（仅 base-container 层）。
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "exec/mxc_ffi.hpp"
#include "exec/process.hpp"
#include "exec/sandbox.hpp"
#include "lib/nlohmann/json.hpp"

namespace dagent::exec {

/// 适配基线的 MXC 版本；加载时与 `mxc_version()` 比较，不匹配即后端不可用。
inline constexpr std::string_view kMxcVersion = "1.0.1";
/// 请求使用的策略契约版本；MXC 只接受精确字符串。
inline constexpr std::string_view kMxcPolicyVersion = "1.0.0";

/// @brief 已加载的 MXC 原生库；进程内共享，按 ffi::Functions 的并发契约使用。
class MxcLibrary {
public:
    /// 加载库、解析全部符号并校验版本；任一步失败抛 ExecError{sandbox}，消息说明文件与原因。
    static std::shared_ptr<const MxcLibrary> load(const std::filesystem::path& file);
    ~MxcLibrary();
    MxcLibrary(const MxcLibrary&) = delete;
    MxcLibrary& operator=(const MxcLibrary&) = delete;

    const std::filesystem::path& file() const noexcept;
    std::string version() const;
    nlohmann::json platform_support() const;   ///< mxc_platform_support_json 的解析结果
    nlohmann::json available_backends() const; ///< mxc_available_backends_json 的解析结果
    /// mxc_probe_request_json_with_error 的解析结果（tier、needsDaclAugmentation、warnings、probes），不创建容器。
    /// 库没有该入口（只有 Windows 构建导出）时返回 nullopt；探测失败抛 ExecError{sandbox}。
    std::optional<nlohmann::json> probe_request(const nlohmann::json& request) const;
    const ffi::Functions& functions() const noexcept; ///< exec 内部使用

private:
    MxcLibrary();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief 启动装配时解析一次的后端资源。
struct MxcRuntime {
    std::shared_ptr<const MxcLibrary> library;
    std::string containment;     ///< "bubblewrap"（Linux）或 "processcontainer"（Windows），装配层按平台填写
    std::filesystem::path shell; ///< 受限命令使用的 managed bash
    std::filesystem::path rg;
};

/// @brief 一次受限执行的输入。文件规则来自中立 Policy；网络由闸门决定。
struct MxcRequest {
    MxcRuntime runtime;
    std::filesystem::path workspace; ///< 命令工作目录（process.cwd），必须在可读或可写范围内
    std::string command;             ///< bash 脚本文本，经 managed bash --noprofile --norc -c 执行
    Policy policy;
    /// 命令环境（PATH、LC_ALL 等）；HOME/XDG/TMPDIR 系列由执行函数指向私有目录。
    std::vector<std::pair<std::string, std::string>> environment;
    std::optional<std::chrono::milliseconds> timeout;
    bool merge_stderr = false; ///< 脚本前置 `exec 2>&1`，交错顺序与宿主执行一致
    std::chrono::milliseconds approval_timeout{120000}; ///< 一次运行中网络审批的等待上限
    int max_network_requests = 32;                      ///< 每次执行向闸门提问的预算
    /// 运行中网络目标判定；为空时命令没有任何网络。policy.network_targets 中的目标不经闸门直接放行。
    NetworkGate network_gate;
};

/// @brief 把中立 Policy 编译为 MXC 1.0.0 策略 JSON：version、containment、filesystem，
/// 以及（proxy_url 非空时）network 与 runtimeConfig；不含 process。Bash、web 与 MCP 共用。
/// 映射：readable → readonlyPaths；writable 与私有 HOME → readwritePaths（read_only 模式不含 writable）；
/// protected_read 与可读树内的敏感路径 → deniedPaths；protected_write 与可写树内的敏感路径 →
/// 嵌套在可写路径内的 readonlyPaths；read_exceptions 从 deniedPaths 剔除。只传已存在的路径。
/// ProcessContainer 另加 `fallback.allowDaclMutation=false`；其代理模式要求 `ingress.default=allow`，
/// 且 DAgent 的代理没有包身份，只能用无身份映射 `ingress.hostLoopback=allow`（WFP 仍只放行到代理端点的出站）。
nlohmann::json mxc_policy(const MxcRuntime& runtime, const Policy& policy,
                          const std::filesystem::path& private_home, std::optional<std::string> proxy_url);

/// @brief 不启动命令，用 run_mxc 会提交的同一份请求文档询问 MXC 的层级选择（Windows 的请求探测）；
/// 私有 HOME 以临时目录代替，network_gate 非空时代理地址用占位端口。库没有探测入口时返回 nullopt。
std::optional<nlohmann::json> probe_mxc(const MxcRequest& request, const Options& options = {});

/// @brief 列表内 `host` 或 `host:port` 直接放行，其余交给 fallback（为空则拒绝）。
NetworkGate allowlist_gate(std::vector<std::string> targets, NetworkGate fallback = {});

/// @brief 在 MXC 后端执行一条命令。语义与 run() 一致：超时置位 timed_out，取消抛 ExecError{cancelled}，
/// 后端或策略失败抛 ExecError{sandbox}；返回前进程树、代理与私有目录都已清理。
/// 闸门返回 cancel 时终止命令并抛 ExecError{cancelled}。
Result run_mxc(const MxcRequest& request, const Options& options = {},
               const std::function<void(Stream, std::string_view)>& on_output = {},
               std::stop_token stop = {});

/// @brief 长生命周期的受限子进程（MCP stdio server）；接口与 exec::Child 一致，回调都在读取线程上触发。
class MxcChild {
public:
    /// 启动受限子进程；失败抛 ExecError{sandbox}。stdin 接管道，stderr 逐行回调或记日志。
    static std::unique_ptr<MxcChild> spawn(const MxcRequest& request, const Options& options = {});
    MxcChild(const MxcChild&) = delete;
    MxcChild& operator=(const MxcChild&) = delete;
    ~MxcChild();

    void write(std::string_view data);
    void on_line(std::function<void(std::string_view)> cb);
    void on_stderr(std::function<void(std::string_view)> cb);
    void on_exit(std::function<void(std::optional<int> code, std::optional<int> signal)> cb);
    /// 终止进程树，等待读取线程结束并清理私有目录；可重复调用。
    void terminate();

private:
    MxcChild();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Shared read-only execution boundary for search and workspace context commands.
struct ReadOnlySandbox {
    std::optional<MxcRuntime> runtime;
    std::vector<std::filesystem::path> protected_read;
    std::vector<std::filesystem::path> read_exceptions;
    Result run(const Command&, const Options& = {},
               const std::function<void(Stream, std::string_view)>& = {}, std::stop_token = {}) const;
};

} // namespace dagent::exec
