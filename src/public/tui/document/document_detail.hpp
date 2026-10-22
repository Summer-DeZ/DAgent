/// @file document_detail.hpp
/// @brief 物化行逐字素遍历：Document 定位（document.cpp）与 Scrollback 选区绘制（scrollback.cpp）共用。
#pragma once

#include <cstddef>
#include <string_view>

#include "tui/document/document.hpp"
#include "tui/grid/grapheme.hpp"

namespace dagent::tui::document_detail {

inline constexpr int k_tab_stop = 8; ///< 与 wrap.cpp 的制表符展开一致

/// @brief 对物化行的每个显示字素回调 f(col, width, byte, content, grapheme, span)；
/// 返回行内内容在源文本中的结束偏移。
template <class F>
size_t walk_line(const Line& ln, std::string_view src, F&& f) {
    int col = 0;
    size_t byte = ln.offset;
    for (size_t k = 0; k < ln.spans.size(); ++k) {
        const Span& sp = ln.spans[k];
        std::string_view d = sp.text;
        unicode::Grapheme g;
        if (sp.src == k_no_src) {
            size_t deco = byte; // 无源装饰锚到其后第一段内容的源偏移
            for (size_t m = k + 1; m < ln.spans.size(); ++m) {
                if (ln.spans[m].src != k_no_src) {
                    deco = ln.spans[m].src;
                    break;
                }
            }
            while (!d.empty() && unicode::next_grapheme(d, g)) {
                f(col, g.width, deco, false, g.bytes, sp);
                col += g.width;
            }
            continue;
        }
        size_t p = sp.src;
        int tab_from = -1;
        while (!d.empty() && unicode::next_grapheme(d, g)) {
            f(col, g.width, p, true, g.bytes, sp);
            col += g.width;
            if (p >= src.size()) continue;
            if (src[p] == '\t') { // 源 '\t' 对应展开出的多个空格
                if (tab_from < 0) tab_from = col - g.width;
                const int stop = (tab_from / k_tab_stop + 1) * k_tab_stop;
                if (col >= stop || d.empty() || d[0] != ' ') {
                    ++p;
                    tab_from = -1;
                }
                continue;
            }
            std::string_view rest = src.substr(p);
            unicode::Grapheme sg;
            p += unicode::next_grapheme(rest, sg) ? sg.bytes.size() : 1;
        }
        byte = p;
    }
    return byte;
}

} // namespace dagent::tui::document_detail
