#pragma once

#include <algorithm>
#include <filesystem>
#include <utility>

namespace dagent::app {

/// One home layout shared by frontend path discovery and backend assembly.
struct HomePaths {
    std::filesystem::path root, config, models, mcp, runtime_config;
    std::filesystem::path prompts, instructions, agents, skills, themes;
    std::filesystem::path memory;
    std::filesystem::path database, decision_database, logs, run, runtime, cache;

    explicit HomePaths(std::filesystem::path directory)
        : root(std::move(directory)), config(root / "config/config.json"),
          models(root / "config/models.json"), mcp(root / "config/mcp.json"),
          runtime_config(root / "config/runtime.json"), prompts(root / "prompts"),
          instructions(root / "AGENTS.md"), agents(root / "agents"), skills(root / "skills"),
          themes(root / "themes"), memory(root / "memory"), database(root / "data/dagent.db"),
          decision_database(root / "data/decision.sqlite"), logs(root / "logs"),
          run(root / "run"), runtime(root / "runtime"), cache(root / "cache/packages") {}
    std::filesystem::path project_memory(const std::filesystem::path& project_root) const {
        auto key = std::filesystem::weakly_canonical(project_root).generic_string();
        std::ranges::replace(key, '/', '-');
        return memory / "projects" / key;
    }
};

} // namespace dagent::app
