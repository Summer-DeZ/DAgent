#include "ui/history_pager.hpp"

#include <utility>

#include "client/client.hpp"
#include "tui/runtime/runtime.hpp"

namespace dagent::ui {

HistoryPager::HistoryPager(client::Client& client, tui::Runtime& rt, std::shared_ptr<CallbackGate> gate,
                           std::string session_id)
    : client_(client), rt_(rt), gate_(std::move(gate)), session_id_(std::move(session_id)) {}

void HistoryPager::start() { request(); }

void HistoryPager::cancel() {
    std::string cursor;
    {
        const std::lock_guard lock(mutex_);
        cancelled_ = true;
        cursor = std::exchange(cursor_, {});
    }
    close_cursor(cursor);
}

void HistoryPager::close_cursor(const std::string& cursor) {
    if (!cursor.empty())
        client_.call_async("session.history_close", {{"cursor", cursor}},
                           [](std::expected<nlohmann::json, protocol::RpcError>) {});
}

void HistoryPager::request() {
    nlohmann::json params{{"session_id", session_id_}, {"limit", 100}};
    {
        const std::lock_guard lock(mutex_);
        if (cancelled_) return;
        // 从这里到响应到达，游标归在途请求所有；cancel 由 handle 关闭响应中的新游标。
        if (!cursor_.empty()) params["cursor"] = std::exchange(cursor_, {});
    }
    auto self = shared_from_this();
    client_.call_async("session.history", std::move(params),
                       [self](std::expected<nlohmann::json, protocol::RpcError> response) {
                           self->handle(std::move(response));
                       });
}

void HistoryPager::handle(std::expected<nlohmann::json, protocol::RpcError> response) {
    const std::lock_guard gate_lock(gate_->mutex);
    if (!response) {
        if (!gate_->open || cancelled()) return;
        auto self = shared_from_this();
        rt_.post([self, message = response.error().message] {
            const std::lock_guard lock(self->gate_->mutex);
            if (!self->gate_->open || self->cancelled()) return;
            if (self->on_error) self->on_error(message);
        });
        return;
    }
    std::vector<protocol::HistoryItem> items;
    for (const auto& item : response->value("items", nlohmann::json::array()))
        items.push_back(item.get<protocol::HistoryItem>());
    std::string next;
    if (const auto it = response->find("next_cursor"); it != response->end() && it->is_string())
        next = it->get<std::string>();
    bool discard;
    {
        const std::lock_guard lock(mutex_);
        discard = cancelled_ || !gate_->open;
        if (!discard) cursor_ = next;
    }
    if (discard) { close_cursor(next); return; }
    auto self = shared_from_this();
    rt_.post([self, items = std::move(items), done = next.empty()]() mutable {
        const std::lock_guard lock(self->gate_->mutex);
        if (!self->gate_->open || self->cancelled()) return;
        for (const auto& item : items) {
            if (self->on_item) self->on_item(item);
        }
        if (!done) self->request();
        else if (self->on_done) self->on_done();
    });
}

bool HistoryPager::cancelled() {
    const std::lock_guard lock(mutex_);
    return cancelled_;
}

std::vector<protocol::HistoryItem> load_history(client::Client& client, const std::string& session_id) {
    std::vector<protocol::HistoryItem> items;
    std::string cursor;
    for (;;) {
        nlohmann::json params{{"session_id", session_id}, {"limit", 100}};
        if (!cursor.empty()) params["cursor"] = cursor;
        const nlohmann::json page = client.call("session.history", std::move(params));
        for (const auto& item : page.value("items", nlohmann::json::array())) {
            items.push_back(item.get<protocol::HistoryItem>());
        }
        if (const auto it = page.find("next_cursor"); it != page.end() && it->is_string()) {
            cursor = it->get<std::string>();
        } else {
            break;
        }
    }
    return items;
}

} // namespace dagent::ui
