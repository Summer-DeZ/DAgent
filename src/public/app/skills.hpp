#pragma once

#include <filesystem>
#include <memory>

#include "agent/skills.hpp"
#include "workspace/files.hpp"

namespace dagent::app {

std::shared_ptr<const agent::SkillCatalog> load_skills(const std::filesystem::path& directory,
                                                     const workspace::FileOptions& files);

} // namespace dagent::app
