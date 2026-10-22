/// @file mxc_ffi.hpp
/// @brief MXC 1.0.1 原生库（libmxc_ffi.so / mxc_ffi.dll）的 C ABI：结构体布局与函数指针类型。
///
/// MXC 不发布 C 头文件，其 C ABI 声明为"随库版本走、不是稳定外部 ABI"。这里按上游
/// `src/ffi/mxc_ffi/src/{lib,streaming}.rs`（v1.0.1）手写；加载时校验 `mxc_version()`，不匹配即不可用。
/// 所有字符串为 UTF-8、NUL 结尾；库返回的字符串用 `string_free` 释放，`version` 的返回值是静态的。
#pragma once

#include <cstddef>
#include <cstdint>

namespace dagent::exec::ffi {

/// 字段为 nullptr 表示"未提供"，绝不是空串。
struct MxcErrorDetail {
    char* message_utf8;
    char* operation_utf8;
    char* native_code_utf8;
    char* remediation_utf8;
};

/// `run_json` 的结果；结构体由调用方提供，`run_result_free` 只释放其中的字符串。
struct MxcRunResult {
    std::int32_t status;
    std::int32_t exit_code; ///< 超时为 -1 且 timed_out=1；信号终止经 sh 表现为 128+N
    std::int32_t timed_out;
    char* stdout_utf8;
    char* stderr_utf8;
    MxcErrorDetail error;
    char* output_metadata_json_utf8;
    char* warnings_json_utf8;
};

/// Unix 为文件描述符，Windows 为句柄；-1 表示没有该流。
struct MxcNativeStdio {
    std::intptr_t stdin_handle;
    std::intptr_t stdout_handle;
    std::intptr_t stderr_handle;
};

struct MxcSandbox;
struct MxcReadStream;
struct MxcWriteStream;
struct MxcStreamCloser;

/// 状态码：0 成功；1–12 对应上游 ErrorCode；100 以上是 FFI 层自己的错误。
enum Status : std::int32_t {
    ok = 0,
    malformed_request = 1,
    unsupported_containment = 2,
    unsupported_phase = 3,
    backend_unavailable = 4,
    malformed_id = 5,
    stale_id = 6,
    not_provisioned = 7,
    not_started = 8,
    already_started = 9,
    already_stopped = 10,
    policy_validation = 11,
    backend_error = 12,
    null_argument = 100,
    invalid_utf8 = 101,
    panic = 102,
};

/// 已解析的导出函数。并发契约（上游 streaming.rs）：同一个 MxcSandbox* 上的调用由调用方串行化，
/// 可取消的等待应在同一线程轮询 try_wait 并调用 kill；stdin/stdout/stderr 是独立对象，可在各自线程上使用；
/// 每个流句柄单一所有者。
struct Functions {
    const char* (*version)();
    char* (*platform_support_json)();
    char* (*available_backends_json)();
    void (*string_free)(char*);
    void (*error_detail_free)(MxcErrorDetail*);

    std::int32_t (*run_json)(const char* request_json, std::int32_t experimental, MxcRunResult* out);
    void (*run_result_free)(MxcRunResult*);

    std::int32_t (*spawn_json)(const char* request_json, std::int32_t experimental, MxcSandbox** out_handle,
                               MxcErrorDetail* out_error);
    MxcWriteStream* (*sandbox_take_stdin)(MxcSandbox*);  ///< 只有第一次调用返回流，之后为 nullptr
    MxcReadStream* (*sandbox_take_stdout)(MxcSandbox*);
    MxcReadStream* (*sandbox_take_stderr)(MxcSandbox*);
    MxcStreamCloser* (*sandbox_stdout_closer)(MxcSandbox*);
    MxcStreamCloser* (*sandbox_stderr_closer)(MxcSandbox*);
    std::int32_t (*stream_read)(MxcReadStream*, std::uint8_t* buffer, std::size_t capacity,
                                std::size_t* out_read); ///< 阻塞；读到 0 字节即 EOF
    std::int32_t (*stream_write)(MxcWriteStream*, const std::uint8_t* data, std::size_t length,
                                 std::size_t* out_written);
    std::int32_t (*stream_flush)(MxcWriteStream*);
    std::int32_t (*stream_closer_close)(MxcStreamCloser*); ///< 让阻塞中的读以 EOF 返回，不终止子进程
    void (*stream_closer_free)(MxcStreamCloser*);
    void (*read_stream_free)(MxcReadStream*);
    void (*write_stream_free)(MxcWriteStream*); ///< 释放 stdin 即向子进程送出 EOF

    std::uint32_t (*sandbox_id)(MxcSandbox*); ///< 子进程（bwrap）的 OS pid
    std::int32_t (*sandbox_try_wait)(MxcSandbox*, std::int32_t* out_exit, std::int32_t* out_running,
                                     std::int32_t* out_timed_out);
    std::int32_t (*sandbox_wait)(MxcSandbox*, std::int32_t* out_exit, std::int32_t* out_timed_out);
    std::int32_t (*sandbox_kill)(MxcSandbox*);             ///< SIGKILL 首进程，再杀整个进程组
    std::int32_t (*sandbox_kill_for_timeout)(MxcSandbox*); ///< 同上，之后 wait 报告 timed_out=1
    void (*sandbox_free)(MxcSandbox*);                     ///< 仍在运行则先杀进程树；只能调用一次
    std::int32_t (*sandbox_warnings_json)(MxcSandbox*, char** out);

    /// 只有 Windows 构建导出，其它平台为 nullptr。按一份完整的一次性 ProcessContainer 请求探测可用层级，
    /// 不创建容器；成功时 *out_json 用 string_free 释放，失败时 out_error 用 error_detail_free 释放。
    std::int32_t (*probe_request_json_with_error)(const char* request_json, char** out_json,
                                                  MxcErrorDetail* out_error);
};

} // namespace dagent::exec::ffi
