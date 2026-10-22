#include "ui/shell_detail.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace dagent::ui {

struct FrontendBridge::Impl {
    std::mutex mutex;
    Shell* shell = nullptr;
};

FrontendBridge::FrontendBridge() : impl_(std::make_unique<Impl>()) {}
FrontendBridge::~FrontendBridge() = default;

void FrontendBridge::attach(Shell& shell) {
    const std::lock_guard lock(impl_->mutex);
    impl_->shell = &shell;
}
void FrontendBridge::detach() {
    const std::lock_guard lock(impl_->mutex);
    impl_->shell = nullptr;
}
void FrontendBridge::event(const protocol::Event& event) {
    Shell* shell = nullptr;
    {
        const std::lock_guard lock(impl_->mutex);
        shell = impl_->shell;
    }
    if (shell) shell->on_backend_event(event);
}
void FrontendBridge::interaction_requested(const protocol::InteractionRequest& request) {
    Shell* shell = nullptr;
    {
        const std::lock_guard lock(impl_->mutex);
        shell = impl_->shell;
    }
    if (shell) shell->on_backend_interaction(request);
}
void FrontendBridge::interaction_closed(const std::string& interaction_id) {
    Shell* shell = nullptr;
    {
        const std::lock_guard lock(impl_->mutex);
        shell = impl_->shell;
    }
    if (shell) shell->on_backend_interaction_closed(interaction_id);
}
void FrontendBridge::disconnected(const std::string& message) {
    Shell* shell = nullptr;
    {
        const std::lock_guard lock(impl_->mutex);
        shell = impl_->shell;
    }
    if (shell) shell->on_backend_disconnected(message);
}

} // namespace dagent::ui
