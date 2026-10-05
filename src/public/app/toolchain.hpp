#pragma once

#include <map>
#include <string>

#include "app/home.hpp"
#include "exec/process.hpp"
#include "lib/nlohmann/json.hpp"

namespace dagent::app {

/// Explicit preparation and immutable startup snapshots of private tool environments.
class Toolchain {
public:
    explicit Toolchain(HomePaths paths);
    nlohmann::json sync();
    nlohmann::json status() const;
    std::map<std::string, exec::Environment> environments() const;
    std::filesystem::path environment_root(std::string_view name) const;
    std::filesystem::path program(std::string_view name) const;

private:
    HomePaths paths_;
    nlohmann::json config_;
    nlohmann::json installed_;
};

} // namespace dagent::app
