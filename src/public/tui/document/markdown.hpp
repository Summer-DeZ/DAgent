/// @file markdown.hpp
/// @brief 流式 Markdown 分块：把增量原文切成 Document 块。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "tui/document/document.hpp"

namespace dagent::tui {

/// @brief 把增量 Markdown 原文切成 Document 块，只有最后一个块增长。
class MarkdownStream {
public:
    /// @param first_margin 每条消息第一块的上边距（消息内其余块为 1）。
    explicit MarkdownStream(Document& doc, uint8_t first_margin = 0) noexcept
        : doc_(&doc), first_margin_(first_margin) {}
    MarkdownStream(const MarkdownStream&) = delete;
    MarkdownStream& operator=(const MarkdownStream&) = delete;

    /// @brief 追加任意分块的原文。
    void feed(std::string_view chunk);
    /// @brief 消息结束：未完成的行保留在块内，关闭最后一个块。
    void finish();

private:
    enum class Mode : uint8_t { none, markdown, code, table };
    enum class Family : uint8_t { text, heading, list, quote };

    void on_partial();              ///< pending_ 增长（尚无换行）
    void on_line(bool has_newline); ///< pending_ 是一整行
    void handle_markdown_line(std::string_view line, bool has_newline);
    void handle_detached_line(std::string_view line, bool has_newline,
                              bool allow_table);
    void open_markdown(std::string source, bool pipe);
    void open_table(std::string source);
    void start_code(char fence, size_t len, std::string info);
    Block new_block(BlockKind kind); ///< 按消息内位置设置上边距
    void close_current();
    void remove_last(size_t bytes);

    Document* doc_;
    Mode mode_ = Mode::none;
    Family family_ = Family::text;
    uint64_t cur_ = 0;            ///< 当前 open 块
    char fence_ch_ = 0;           ///< 代码围栏字符
    size_t fence_len_ = 0;        ///< 围栏长度
    bool code_candidate_ = false; ///< 未完成行可能是闭合围栏
    bool line_in_block_ = false;  ///< 当前行已（部分）写入当前块
    bool line_new_ = false;       ///< 当前行的块是行中途新建的
    bool last_line_pipe_ = false; ///< 当前 markdown 块最后一行以 '|' 开头
    size_t last_line_len_ = 0;    ///< 该行字节数（含换行）
    size_t sent_ = 0;             ///< pending_ 中已追加的字节数
    std::string pending_;         ///< 未完成行（不含换行）
    std::string staged_;          ///< 暂存的表格候选表头（含换行）
    bool staging_ = false;
    bool started_ = false; ///< 本条消息已建过块
    uint8_t first_margin_ = 0;
};

} // namespace dagent::tui
