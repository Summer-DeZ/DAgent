/// @file login.hpp
/// @brief backend 的登录端口；实现位于 app，返回值不包含凭据。
#pragma once

#include <stop_token>
#include <string_view>

#include "lib/nlohmann/json.hpp"

namespace dagent::backend {

// Backend-only login operations; implementations never return credentials.
class LoginGateway {
public:
    virtual ~LoginGateway() = default;
    virtual nlohmann::json invoke(std::string_view method, const nlohmann::json&, std::stop_token) = 0;
    virtual bool cancel(std::string_view login_id) = 0;
};

} // namespace dagent::backend
