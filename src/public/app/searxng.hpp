#pragma once

#include <memory>
#include <stop_token>
#include <string>
#include "app/home.hpp"
#include "exec/process.hpp"
#include "web/web.hpp"

namespace dagent::app {

// One lazy search service per backend, shared by its session contexts.
class Searxng {
public:
    Searxng(HomePaths, std::filesystem::path environment, web::Options, exec::Options);
    ~Searxng();
    std::string endpoint(std::stop_token);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dagent::app
