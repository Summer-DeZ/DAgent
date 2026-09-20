#pragma once

#include <map>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <chrono>

#include "agent/events.hpp"
#include "tui/document.hpp"

namespace dagent::ui {

/// 只在渲染线程使用；实时和恢复事件共用这一个文档投影。
class Transcript {
public:
    explicit Transcript(tui::Document&, std::function<void(const tools::TodoView&)> todo = {});
    void apply(const agent::Event&);
    void clear();
    void toggle_tools();
    void toggle_thoughts();
    void set_session(std::string mode, std::string model);
    void info(std::string text);
    void banner(std::string version, std::string cwd, std::string branch,
                std::string model, int mcp_servers);
    void resumed(std::string id, std::size_t messages, std::string age);
    void set_todo_narrow(bool);
    void set_todo_collapsed(bool);

private:
    struct ToolBlocks {
        uint64_t title = 0, body = 0, fold = 0;
        uint32_t rows = 3;
        uint32_t group = 0;
        bool finished = false;
        bool foldable = false;
        std::string label;
        std::chrono::steady_clock::time_point began{};
    };
    /// 一段思考：标题行、正文块与耗时，供 ctrl+r 统一展开收起。
    struct Thought {
        uint64_t title = 0, body = 0;
        double seconds = 0;
    };
    uint64_t text(std::string source, std::string meta, bool open = false);
    void finish_thought();
    void finish_message();
    ToolBlocks& tool(const std::string& id, const std::string& name, const std::string& summary);
    void finished(const agent::ToolFinished&);
    void collapse(ToolBlocks&);
    void update_todo_block();

    tui::Document& doc_;
    std::unique_ptr<tui::MarkdownStream> markdown_;
    uint64_t reasoning_ = 0, step_start_ = 0;
    uint64_t thought_title_ = 0;
    std::vector<Thought> thoughts_;
    std::chrono::steady_clock::time_point thought_began_{}, turn_began_{};
    std::string mode_, model_;
    bool expanded_ = false, thoughts_expanded_ = false;
    bool live_step_ = false, has_text_ = false;
    std::map<std::string, ToolBlocks> tools_;
    std::function<void(const tools::TodoView&)> on_todo_;
    tools::TodoView todo_;
    uint64_t todo_block_ = 0;
    uint32_t next_group_ = 1;
    bool todo_narrow_ = false, todo_collapsed_ = false, todo_complete_ = false;
};

} // namespace dagent::ui
