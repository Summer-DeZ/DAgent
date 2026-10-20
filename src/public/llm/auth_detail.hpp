/// @file auth_detail.hpp
/// @brief llm 登录流程与平台凭据存储共用的内部接口。
#pragma once

#include <memory>

#include "llm/auth.hpp"

namespace dagent::llm::detail {

void check_stop(std::stop_token stop);
void pause(std::chrono::steady_clock::duration duration, std::stop_token stop);
nlohmann::json credential_json(const Credentials& value);
Credentials decode_credentials(const std::string& data);

class CredentialLock {
public:
    CredentialLock(const CredentialPaths& paths, std::stop_token stop);
    ~CredentialLock();
    CredentialLock(const CredentialLock&) = delete;
    CredentialLock& operator=(const CredentialLock&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void write_credentials(const CredentialPaths& paths, const Credentials& value);
void remove_credentials(const CredentialPaths& paths);

} // namespace dagent::llm::detail
