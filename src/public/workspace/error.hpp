/// @file error.hpp
/// @brief workspace 模块唯一的错误类型。
#pragma once

#include <stdexcept>
#include <string>

namespace dagent::workspace {

/// @brief 文件、搜索、上下文采集失败。Kind 按「调用方要不要换一种处理方式」划分。
class WorkspaceError : public std::runtime_error {
public:
    enum class Kind {
        io,           ///< 读写系统调用失败：不存在、权限、目标不可用
        stale,        ///< 写入前发现文件被其他人改过
        too_large,    ///< 内容超过读写上限
        not_text,     ///< 对二进制文件调用文本接口
        tool_missing, ///< 运行时依赖（rg、git）找不到
        bad_pattern,  ///< rg 正则错误
        bad_template, ///< inja 模板错误
        cancelled,    ///< stop_token 请求停止
    };

    WorkspaceError(Kind kind, const std::string& what) : std::runtime_error(what), kind_(kind) {}
    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

} // namespace dagent::workspace
