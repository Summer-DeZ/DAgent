#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dagent::agent {

struct SkillDefinition {
    std::string name, description;
    std::filesystem::path file;
    std::string body, license, compatibility, allowed_tools;
    std::map<std::string, std::string> metadata;
};

struct SkillDiagnostic {
    std::string path, message;
};

/// 启动装配后通过 shared_ptr<const SkillCatalog> 共享，不包含运行期激活状态。
struct SkillCatalog {
    std::vector<SkillDefinition> definitions;
    std::vector<SkillDiagnostic> diagnostics;

    const SkillDefinition* find(std::string_view name) const;
    std::string prompt() const;
};

bool valid_skill_name(std::string_view name);
/// 提取独立的 $name；跳过转义、行内代码和 fenced code，保留首次出现顺序。
std::vector<std::string> skill_mentions(std::string_view text);

/// 只由执行线程持有；引用的定义由会话共享的不可变目录保证寿命。
class RunSkills {
public:
    bool contains(std::string_view name) const;
    void activate(const SkillDefinition& skill);
    std::string context() const;

private:
    std::vector<const SkillDefinition*> active_;
};

} // namespace dagent::agent
