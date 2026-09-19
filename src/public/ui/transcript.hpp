#pragma once

#include <map>
#include <memory>
#include <string>

#include "agent/events.hpp"
#include "tui/document.hpp"

namespace dagent::ui {

/// 只在渲染线程使用；实时和恢复事件共用这一个文档投影。
class Transcript {
public:
    explicit Transcript(tui::Document&);
    void apply(const agent::Event&);
    void clear();
    void toggle_tools();
    void info(std::string text);

private:
    struct ToolBlocks {
        uint64_t title = 0, body = 0;
        uint32_t rows = 3;
        bool finished = false;
        bool foldable = false;
        std::string label;
    };
    uint64_t text(std::string source, std::string meta, bool open = false);
    void finish_message();
    ToolBlocks& tool(const std::string& id, const std::string& name, const std::string& summary);
    void finished(const agent::ToolFinished&);
    void collapse(ToolBlocks&);

    tui::Document& doc_;
    std::unique_ptr<tui::MarkdownStream> markdown_;
    uint64_t reasoning_ = 0, step_start_ = 0;
    bool expanded_ = false;
    bool live_step_ = false, has_text_ = false;
    std::map<std::string, ToolBlocks> tools_;
};

} // namespace dagent::ui
