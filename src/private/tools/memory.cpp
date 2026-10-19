#include "tools/memory.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

#include "base/log.hpp"
#include "workspace/files.hpp"
#include "tools/detail.hpp"

namespace dagent::tools {
namespace {
namespace fs = std::filesystem;

bool valid_name(std::string_view name) {
    if (name.empty() || name.front() == '-' || name.back() == '-') return false;
    bool hyphen = false;
    for (char c : name) {
        if (c == '-' && hyphen) return false;
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-') return false;
        hyphen = c == '-';
    }
    return true;
}

void validate(std::string_view type, std::string_view description) {
    if (type != "user" && type != "feedback" && type != "project" && type != "reference")
        throw std::runtime_error("type must be user, feedback, project or reference");
    if (description.find_first_not_of(" \t\r\n") == std::string_view::npos ||
        description.find_first_of("\r\n") != std::string_view::npos)
        throw std::runtime_error("description must be a non-empty single line");
}

std::string scalar(const YAML::Node& node, const char* key) {
    const auto value = node[key];
    if (!value || !value.IsScalar()) throw std::runtime_error(std::string(key) + " must be a string");
    return value.as<std::string>();
}

bool valid_date(std::string_view date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') return false;
    for (std::size_t i = 0; i < date.size(); ++i)
        if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) return false;
    const auto number = [&](std::size_t offset, std::size_t count) {
        return std::stoi(std::string(date.substr(offset, count)));
    };
    return std::chrono::year_month_day{std::chrono::year(number(0, 4)),
        std::chrono::month(number(5, 2)), std::chrono::day(number(8, 2))}.ok();
}
} // namespace

MemoryStore::MemoryStore(fs::path root, fs::path project, MemoryOptions options)
    : root_(fs::weakly_canonical(root.parent_path()) / root.filename()),
      project_(std::move(project)), options_(options) {}

fs::path MemoryStore::directory(std::string_view scope) const {
    fs::path path;
    if (scope == "global") path = root_ / "global";
    else if (scope == "project") path = project_;
    else throw std::runtime_error("scope must be global or project");
    return path;
}

fs::path MemoryStore::file(std::string_view scope, std::string_view name) const {
    if (!valid_name(name)) throw std::runtime_error("name must use kebab-case lowercase letters and digits");
    const auto path = directory(scope) / (std::string(name) + ".md");
    const auto status = fs::symlink_status(path);
    if (fs::exists(status) && !fs::is_regular_file(status))
        throw std::runtime_error("memory entry must be a regular file; symbolic links and other file types are not allowed");
    return path;
}

MemoryEntry MemoryStore::view(std::string_view scope, std::string_view name) const {
    const auto input = workspace::FileReference(file(scope, name)).read({.max_read_bytes = options_.max_entry_bytes});
    if (input.truncated || input.lossy) throw std::runtime_error("memory entry exceeds max_entry_bytes or is not valid UTF-8");
    const auto& text = input.content;
    if (!text.starts_with("---\n")) throw std::runtime_error("missing YAML frontmatter");
    std::size_t end = 4, body = std::string::npos;
    while (end < text.size()) {
        const auto newline = text.find('\n', end);
        const auto last = newline == std::string::npos ? text.size() : newline;
        if (std::string_view(text).substr(end, last - end) == "---") {
            body = newline == std::string::npos ? last : newline + 1;
            break;
        }
        if (newline == std::string::npos) break;
        end = newline + 1;
    }
    if (body == std::string::npos) throw std::runtime_error("missing closing YAML delimiter");
    const auto header = YAML::Load(text.substr(4, end - 4));
    if (!header.IsMap()) throw std::runtime_error("frontmatter must be a mapping");
    MemoryEntry entry{std::string(scope), scalar(header, "name"), scalar(header, "type"),
                      scalar(header, "updated"), scalar(header, "description"), text.substr(body)};
    if (entry.name != name) throw std::runtime_error("name must match the filename");
    validate(entry.type, entry.description);
    if (!valid_date(entry.updated)) throw std::runtime_error("updated must be an absolute YYYY-MM-DD date");
    return entry;
}

std::vector<MemoryEntry> MemoryStore::list() const {
    std::vector<MemoryEntry> entries;
    for (const auto scope : {"global", "project"}) {
        try {
            const auto dir = directory(scope);
            const workspace::FileReference reference(dir);
            if (reference.kind() == workspace::FileKind::missing) continue;
            for (const auto& candidate : fs::directory_iterator(reference.directory_path())) {
                if (candidate.path().extension() != ".md") continue;
                try { entries.push_back(view(scope, candidate.path().stem().string())); }
                catch (const std::exception& error) {
                    base::logger("tools")->warn("Memory {}: {}", (dir / candidate.path().filename()).string(), error.what());
                }
            }
        } catch (const std::exception& error) {
            base::logger("tools")->warn("Memory {}: {}", scope, error.what());
        }
    }
    std::ranges::sort(entries, [](const auto& a, const auto& b) {
        return std::tie(a.scope, a.name) < std::tie(b.scope, b.name);
    });
    return entries;
}

std::string MemoryStore::prompt() const {
    auto entries = list();
    // 淘汰顺序：全局先于项目，同组旧日期先于新日期；同日按名字固定顺序。
    std::ranges::sort(entries, [](const auto& a, const auto& b) {
        return std::tie(a.scope, a.updated, a.name) < std::tie(b.scope, b.updated, b.name);
    });
    const auto line = [](const MemoryEntry& entry) {
        return std::format("- {} / {} / {} / {}\n", entry.name, entry.type, entry.updated, entry.description);
    };
    constexpr std::string_view global = "## Global\n", project = "\n## Project\n";
    std::size_t bytes = global.size() + project.size();
    for (const auto& entry : entries) bytes += line(entry).size();
    std::size_t first = 0;
    while (bytes > options_.max_index_bytes && first < entries.size())
        bytes -= line(entries[first++]).size();
    std::string index;
    if (bytes <= options_.max_index_bytes) {
        for (const auto scope : {"global", "project"}) {
            index += scope == std::string_view("global") ? global : project;
            for (std::size_t i = entries.size(); i > first; --i)
                if (entries[i - 1].scope == scope) index += line(entries[i - 1]);
        }
    }
    return std::string(R"(

# Memory
Memories record what earlier sessions learned. Conventions and practices the user wants followed are standing instructions whatever their type (user, feedback or project): apply them by default to all code and work they cover, including short standalone snippets, even when a convention looks unusual. Set one aside only when the user explicitly asks for something it forbids or AGENTS.md says otherwise; naming something in a request or asking for a short answer is not a conflict. Recalled facts such as file paths, functions, configuration or past decisions are background: verify them before relying on them. Priority: current user requests > project AGENTS.md > global AGENTS.md > memories.
Use the memory tool to view a body, save an entry, or delete an outdated or incorrect entry. Global scope is for cross-project facts; project scope is for this project.

Writing guidelines:
- Save user profile and communication preferences; user corrections and endorsed practices with their reasons; project decisions, constraints and goals that cannot be inferred from code or git; locations of external resources.
- Do not save code structure or function locations, git history, information already in AGENTS.md or documentation, current task progress or temporary state, debugging steps or one-off fixes, secrets, credentials, sensitive personal information, or assumptions not confirmed by the user.
- Write description as one sentence stating the actual point, for example "Do not add Co-Authored-By attribution to commits or PRs", rather than a title such as "Commit conventions".
- Before saving, check this index for an existing entry on the same topic. View and update it instead of creating a duplicate. Delete outdated or incorrect entries.
- For feedback and project entries, include Why and How to apply. Write deadlines as absolute dates.
- Record conventions the user states as stated; do not drop or alter one because you cannot verify it.

The index below contains name / type / updated / description, grouped by scope. Bodies are available through memory(op="view", scope=..., name=...). The index stays fixed while this session is open; saved entries enter the index on the next creation or resume. A byte budget may omit entries, so this index may be incomplete.

)") + index;
}

MemoryEntry MemoryStore::save(std::string_view scope, std::string_view name, std::string_view type,
                              std::string_view description, std::string_view content) const {
    const auto path = file(scope, name);
    validate(type, description);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_r(&now, &local);
    char today[16] = {};
    std::strftime(today, sizeof(today), "%Y-%m-%d", &local);
    MemoryEntry entry{std::string(scope), std::string(name), std::string(type), today,
                      std::string(description), std::string(content)};
    YAML::Emitter header;
    header << YAML::BeginMap << YAML::Key << "name" << YAML::Value << entry.name
           << YAML::Key << "description" << YAML::Value << entry.description
           << YAML::Key << "type" << YAML::Value << entry.type
           << YAML::Key << "updated" << YAML::Value << entry.updated << YAML::EndMap;
    const auto text = std::string("---\n") + header.c_str() + "\n---\n" + entry.content;
    workspace::FileReference(path).write(text, workspace::Eol::lf, false, {},
                                         {.max_write_bytes = options_.max_entry_bytes});
    return entry;
}

void MemoryStore::erase(std::string_view scope, std::string_view name) const {
    workspace::FileReference(file(scope, name)).erase();
}

namespace {
class MemoryCall final : public PreparedTool {
public:
    MemoryCall(std::shared_ptr<const MemoryStore> store, std::string op, MemoryEntry entry)
        : store_(std::move(store)), op_(std::move(op)), entry_(std::move(entry)) {
        intent_.kind = agent::ToolKind::memory;
        intent_.summary = std::format("Memory {} {}/{}", op_, entry_.scope, entry_.name);
    }

private:
    Result do_execute(const Grant&, const std::function<void(std::string_view)>&, std::stop_token) override {
        Result result;
        try {
            if (op_ == "view") entry_ = store_->view(entry_.scope, entry_.name);
            else if (op_ == "save")
                entry_ = store_->save(entry_.scope, entry_.name, entry_.type, entry_.description, entry_.content);
            else store_->erase(entry_.scope, entry_.name);
            result.model_text = op_ == "view" ? entry_.content : intent_.summary;
        } catch (const std::exception& error) {
            result = detail::error_result(error.what());
        }
        result.display = agent::MemoryView{op_, entry_.scope, entry_.name, entry_.type,
            entry_.updated, entry_.description, entry_.content};
        return result;
    }
    std::shared_ptr<const MemoryStore> store_;
    std::string op_;
    MemoryEntry entry_;
};

class MemoryTool final : public Tool {
public:
    explicit MemoryTool(std::shared_ptr<const MemoryStore> store) : store_(std::move(store)) {
        spec_.name = "memory";
        spec_.description = R"(Read or maintain persistent global and project memories.
Use view to read an entry's body, save to create or replace it, and delete to remove an outdated entry.
Names must be kebab-case. Save requires type, description and content; the program writes frontmatter and the updated date.
Types: user (profile/preferences), feedback (corrections or endorsed practices), project (decisions/constraints/goals), reference (external resources).
For feedback and project entries include Why and How to apply. Use absolute dates for deadlines.
Global scope is for cross-project preferences; project scope is for this project's facts.
Follow the # Memory guidelines. Operations run serially and need no approval, including in planning mode.)";
        spec_.parameters = {
            {"type", "object"},
            {"properties", {
                {"op", {{"type", "string"}, {"enum", {"view", "save", "delete"}}}},
                {"scope", {{"type", "string"}, {"enum", {"global", "project"}}}},
                {"name", {{"type", "string"}, {"pattern", "^[a-z0-9]+(-[a-z0-9]+)*$"}}},
                {"type", {{"type", "string"}, {"enum", {"user", "feedback", "project", "reference"}}}},
                {"description", {{"type", "string"}}},
                {"content", {{"type", "string"}}}}},
            {"required", {"op", "scope", "name"}}, {"additionalProperties", false}};
    }
    const Spec& spec() const override { return spec_; }
    std::expected<std::unique_ptr<PreparedTool>, Result> prepare(
        std::string_view arguments, Context&) const override {
        const auto args = detail::parse_arguments(arguments);
        if (!args) return std::unexpected(detail::error_result(args.error()));
        std::string error;
        auto op = detail::require_string(*args, "op", error);
        MemoryEntry entry;
        entry.scope = detail::require_string(*args, "scope", error);
        entry.name = detail::require_string(*args, "name", error);
        if (op == "save") {
            entry.type = detail::require_string(*args, "type", error);
            entry.description = detail::require_string(*args, "description", error);
            entry.content = detail::require_string(*args, "content", error);
        }
        if (op != "save" && op != "view" && op != "delete") error = "op must be view, save or delete";
        if (entry.scope != "global" && entry.scope != "project") error = "scope must be global or project";
        if (!valid_name(entry.name)) error = "name must use kebab-case lowercase letters and digits";
        if (!error.empty()) return std::unexpected(detail::error_result(error));
        return std::make_unique<MemoryCall>(store_, std::move(op), std::move(entry));
    }
private:
    Spec spec_;
    std::shared_ptr<const MemoryStore> store_;
};
} // namespace

std::unique_ptr<Tool> make_memory_tool(std::shared_ptr<const MemoryStore> store) {
    return std::make_unique<MemoryTool>(std::move(store));
}

} // namespace dagent::tools
