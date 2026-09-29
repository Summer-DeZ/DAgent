/// @file files.hpp
/// @brief 文件原语：路径解析、文本读取、原子写入、stale 检测。不做目录遍历（交给 search）。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <string_view>

#include "workspace/error.hpp"

namespace dagent::workspace {

/// @brief 文件选项，对应 config/dagent.json 的 "files" 段。
struct FileOptions {
    std::size_t max_read_bytes = 8 << 20;   ///< read 工具能翻页读取的最大文件，超出截断并置 truncated
    std::size_t max_write_bytes = 1 << 20;  ///< 写入上限，超出抛 too_large；edit 要先把整个文件读进来
};

struct Resolved {
    std::filesystem::path path; ///< 绝对路径，已经规范化，符号链接已解析
    bool inside_workspace;      ///< 在工作区外时要不要放行，由核心做权限决策
};

/// @brief 把 user_path 解析成绝对路径：相对路径基于 root；两侧都做规范化与符号链接解析。
/// 前缀按路径分量逐个比较，/work2 不会被判成 /work 的子路径。
Resolved resolve(const std::filesystem::path& root, std::string_view user_path);

enum class Eol { lf, crlf, mixed, none };

/// @brief 读取时的文件指纹，写入前拿来比对。
struct Stamp {
    std::filesystem::file_time_type mtime;
    std::uintmax_t size = 0;
    std::uint64_t device = 0, inode = 0;
    bool operator==(const Stamp&) const = default;
};

struct TextFile {
    std::string content; ///< 已统一为 LF、去掉 BOM、非法 UTF-8 替换成 U+FFFD
    Eol eol = Eol::none;
    bool bom = false, lossy = false, truncated = false;
    Stamp stamp;
};

enum class FileKind { text, binary, missing, directory };

/// Metadata-only reference captured before approval. All later I/O is relative to the pinned
/// directory; replacement of a parent or target requires preparing the operation again.
class FileReference {
public:
    explicit FileReference(const std::filesystem::path& canonical_path);
    ~FileReference();
    FileReference(FileReference&&) noexcept;
    FileReference& operator=(FileReference&&) noexcept;
    FileReference(const FileReference&) = delete;
    FileReference& operator=(const FileReference&) = delete;
    FileKind kind() const;
    std::optional<Stamp> stamp() const;
    void validate() const;
    TextFile read(const FileOptions&) const;
    Stamp write(std::string_view, Eol, bool, const std::optional<Stamp>&, const FileOptions&);
    std::filesystem::path directory_path() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief 看前 8 KiB 里有没有 NUL 字节。文件不存在返回 missing，不抛异常。
FileKind probe(const std::filesystem::path&);

/// @brief 读取文本文件。文件不存在、不可读抛 io；目录抛 io。超过 max_read_bytes 时截断并置 truncated。
TextFile read_text(const std::filesystem::path&, const FileOptions& = {});

/// @brief 原子写入：把 LF 内容还原成 eol 和 bom，写临时文件 → fsync → rename，新文件自动建父目录。
/// 目标是符号链接时写到它指向的真实文件上。给了 expect 时，当前 Stamp 不一致就抛 stale。
void write_text(const std::filesystem::path&, std::string_view content, Eol eol, bool bom,
                const std::optional<Stamp>& expect = {}, const FileOptions& = {});

/// @brief 取当前文件的指纹；文件不存在或不可 stat 时返回 nullopt。
std::optional<Stamp> stamp_of(const std::filesystem::path&);

} // namespace dagent::workspace
