#pragma once

#include <mutex>
#include "app/home.hpp"
#include "llm/auth.hpp"
#include "runtime/factory.hpp"

namespace dagent::app {
class Login final : public runtime::LoginGateway {
public:
    explicit Login(HomePaths paths) : paths_(std::move(paths)) {}
    nlohmann::json invoke(std::string_view method, const nlohmann::json&, std::stop_token) override;
    bool cancel(std::string_view login_id) override;
private:
    struct Flow {
        std::string id;
        llm::DeviceLogin device;
        std::stop_source stop;
    };
    llm::CredentialPaths credentials() const;
    nlohmann::json models(bool refresh, std::stop_token);
    HomePaths paths_;
    std::mutex mutex_;
    std::shared_ptr<Flow> flow_;
    std::uint64_t serial_ = 0;
};
} // namespace dagent::app
