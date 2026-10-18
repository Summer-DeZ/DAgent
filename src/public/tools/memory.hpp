#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dagent::tools {

struct MemoryOptions {
    bool enabled = true;
    std::size_t max_index_bytes = 8192;
    std::size_t max_entry_bytes = 4096;
};

struct MemoryEntry {
    std::string scope, name, type, updated, description, content;
};

/// Home 下的全局与当前项目记忆；不缓存索引，允许用户直接编辑文件。
class MemoryStore {
public:
    MemoryStore(std::filesystem::path root, std::filesystem::path project, MemoryOptions options = {});
    std::vector<MemoryEntry> list() const;
    std::string prompt() const;
    MemoryEntry view(std::string_view scope, std::string_view name) const;
    MemoryEntry save(std::string_view scope, std::string_view name, std::string_view type,
                     std::string_view description, std::string_view content) const;
    void erase(std::string_view scope, std::string_view name) const;

private:
    std::filesystem::path directory(std::string_view scope) const;
    std::filesystem::path file(std::string_view scope, std::string_view name) const;
    std::filesystem::path root_, project_;
    MemoryOptions options_;
};

class Tool;
std::unique_ptr<Tool> make_memory_tool(std::shared_ptr<const MemoryStore>);

} // namespace dagent::tools
