#pragma once

#include <map>
#include <set>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <chrono>
#include <filesystem>

#include "protocol/dto.hpp"
#include "tui/document.hpp"
#include "ui/projection.hpp"

namespace dagent::ui {

/// 只在渲染线程使用；实时和恢复历史共用这一个文档投影。
class Transcript {
public:
    /// @brief 一次已结束的 task：切进子会话时用 session_id 回放。
    struct TaskRef {
        std::string agent, session_id, call_id;
        bool failed = false;
        double seconds = 0;
        std::string description = {};
    };

    explicit Transcript(tui::Document&, std::function<void(const TodoList&)> todo = {},
                        std::function<void(const FileChangeView&)> changed = {},
                        std::function<void(const std::string&)> user = {});
    /// @brief 实时事件：当前 Run 的流式投影。
    void apply_live(const Event&);
    /// @brief 历史条目：静态投影，不触发当前 Run 的收尾/计时语义。
    void append_history(const protocol::HistoryItem&);
    void clear();
    void set_compact(bool);
    void refresh_icons();
    void tick();
    void set_render_options(bool auto_diff, bool unified, bool links, std::filesystem::path project);
    bool can_switch_diff(const tui::Scrollback&, bool visible_only = true) const;

    void apply_child(const std::string& call_id, const Event&, const Transcript& child);
    void show_question(const QuestionRequest&);
    void toggle_tools();
    void toggle_thoughts();
    void set_session(std::string mode, std::string model);
    void info(std::string text);
    void banner(std::string version, std::string cwd, std::string branch, int mcp_servers);
    void resumed(std::string id, std::size_t messages, std::string age);
    const std::vector<TaskRef>& tasks() const { return tasks_; }

private:
    struct ToolBlocks {
        uint64_t title = 0, body = 0, fold = 0;
        uint32_t rows = 3;
        uint32_t group = 0;
        int diff_width = 0;
        bool finished = false;
        bool task = false;
        std::string label;
        std::string ask_prompt;
        std::string task_agent, task_description, task_result;
        std::vector<std::pair<std::string, std::string>> children;
        std::chrono::steady_clock::time_point began{};
    };
    /// 一段思考：标题行、正文块与耗时，供 ctrl+r 统一展开收起。
    struct Thought {
        uint64_t title = 0, body = 0;
        double seconds = 0;
    };
    uint64_t text(std::string source, std::string meta, bool open = false);
    void apply(const Event&);
    void finish_thought();
    void finish_message();
    ToolBlocks& tool(const std::string& id, const std::string& name, const std::string& summary);
    void finished(const ToolFinished&);
    void collapse(ToolBlocks&);
    void refresh_task_title(ToolBlocks&);
    void refresh_task_body(ToolBlocks&);

    void refresh_density();
    void install_renderers();
    bool auto_diff_ = true, unified_ = false, links_ = true;
    std::filesystem::path project_;
    int frame_ = 0;
    tui::Document& doc_;
    std::map<uint64_t, uint8_t> margins_;
    std::set<uint64_t> assistant_starts_;
    bool assistant_start_pending_ = true, has_user_ = false;
    std::map<uint64_t, std::function<std::string()>> icon_sources_;
    uint64_t density_end_ = 0;
    bool compact_ = false;
    std::function<void(const FileChangeView&)> on_changed_;
    std::function<void(const std::string&)> on_user_;
    std::unique_ptr<tui::MarkdownStream> markdown_;
    uint64_t reasoning_ = 0, step_start_ = 0;
    uint64_t thought_title_ = 0;
    std::vector<Thought> thoughts_;
    std::chrono::steady_clock::time_point thought_began_{}, turn_began_{};
    std::string mode_, model_;
    bool expanded_ = false, thoughts_expanded_ = false;
    bool live_step_ = false;
    std::map<std::string, ToolBlocks> tools_;
    std::vector<TaskRef> tasks_;
    std::function<void(const TodoList&)> on_todo_;
    uint32_t next_group_ = 1;
};

} // namespace dagent::ui
