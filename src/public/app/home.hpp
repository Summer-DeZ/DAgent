#pragma once

#include <filesystem>
#include <utility>

namespace dagent::app {

/// One home layout shared by frontend path discovery and backend assembly.
struct HomePaths {
    std::filesystem::path root, config, models, mcp, runtime_config;
    std::filesystem::path prompts, instructions, agents, skills, themes;
    std::filesystem::path database, logs, run, runtime, cache;

    explicit HomePaths(std::filesystem::path directory)
        : root(std::move(directory)), config(root / "config/config.json"),
          models(root / "config/models.json"), mcp(root / "config/mcp.json"),
          runtime_config(root / "config/runtime.json"), prompts(root / "prompts"),
          instructions(root / "AGENTS.md"), agents(root / "agents"), skills(root / "skills"),
          themes(root / "themes"), database(root / "data/dagent.db"), logs(root / "logs"),
          run(root / "run"), runtime(root / "runtime"), cache(root / "cache/packages") {}
};

} // namespace dagent::app
