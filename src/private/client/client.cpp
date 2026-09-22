#include "client/client.hpp"

#include <format>
#include <utility>

namespace dagent::client {

struct Client::Pending {
    std::function<void(std::expected<nlohmann::json, protocol::RpcError>)> done;
};

struct Client::SyncPending {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    nlohmann::json result;
    std::optional<protocol::RpcError> error;
    std::string connection_error;
};

Client::Client(ipc::Channel channel, Callbacks callbacks)
    : channel_(std::move(channel)), callbacks_(std::move(callbacks)) {
    reader_ = std::jthread([this](std::stop_token stop) { reader(stop); });
}

Client::~Client() { close(); }

bool Client::connected() const {
    const std::lock_guard lock(mutex_);
    return !closing_ && error_.empty();
}

nlohmann::json Client::call(const std::string& method, nlohmann::json params) {
    auto state = std::make_shared<SyncPending>();
    std::string id;
    {
        const std::lock_guard lock(mutex_);
        if (closing_ || !error_.empty()) throw std::runtime_error(error_.empty() ? "connection closed" : error_);
        id = std::format("r-{}", ++next_id_);
        sync_[id] = state;
    }
    if (!channel_.send_line(protocol::encode_request({id, method, std::move(params)}))) {
        const std::lock_guard lock(mutex_);
        sync_.erase(id);
        throw std::runtime_error(channel_.error().empty() ? "failed to send the request" : channel_.error());
    }
    std::unique_lock lock(state->mutex);
    state->cv.wait(lock, [&] { return state->ready; });
    if (!state->connection_error.empty()) throw std::runtime_error(state->connection_error);
    if (state->error) throw RpcFailure(*state->error);
    return state->result;
}

void Client::call_async(
    std::string method, nlohmann::json params,
    std::function<void(std::expected<nlohmann::json, protocol::RpcError>)> done) {
    auto pending = std::make_shared<Pending>();
    pending->done = std::move(done);
    std::string id;
    std::string closed_error;
    {
        const std::lock_guard lock(mutex_);
        if (closing_ || !error_.empty()) {
            closed_error = error_.empty() ? "connection closed" : error_;
        } else {
            id = std::format("r-{}", ++next_id_);
            pending_[id] = pending;
        }
    }
    if (!closed_error.empty()) {
        protocol::RpcError rpc;
        rpc.code = protocol::rpc_code::kInternalError;
        rpc.message = std::move(closed_error);
        pending->done(std::unexpected(std::move(rpc)));
        return;
    }
    if (!channel_.send_line(protocol::encode_request({id, method, std::move(params)}))) {
        {
            const std::lock_guard lock(mutex_);
            pending_.erase(id);
        }
        protocol::RpcError rpc;
        rpc.code = protocol::rpc_code::kInternalError;
        rpc.message = channel_.error().empty() ? "failed to send the request" : channel_.error();
        pending->done(std::unexpected(std::move(rpc)));
    }
}

void Client::notify(const std::string& method, nlohmann::json params) {
    const std::lock_guard lock(mutex_);
    if (closing_ || !error_.empty()) return;
    // 通知没有响应；发送失败由读取线程统一报告。
    channel_.send_line(protocol::encode_notification({method, std::move(params)}));
}

void Client::close() {
    {
        const std::lock_guard lock(mutex_);
        if (closing_) return;
        closing_ = true;
    }
    finish_all("connection closed", false);
    channel_.shutdown();
    reader_.request_stop();
    if (reader_.joinable()) reader_.join();
}

void Client::finish_all(const std::string& error, bool notify_disconnect) {
    std::map<std::string, std::shared_ptr<Pending>> pending;
    std::map<std::string, std::shared_ptr<SyncPending>> sync;
    std::string message;
    {
        const std::lock_guard lock(mutex_);
        if (!error_.empty()) return;
        error_ = error;
        pending.swap(pending_);
        sync.swap(sync_);
        message = error_;
    }
    if (notify_disconnect && callbacks_.disconnected) callbacks_.disconnected(message);
    for (auto& [id, item] : pending) {
        protocol::RpcError rpc;
        rpc.code = protocol::rpc_code::kInternalError;
        rpc.message = message;
        item->done(std::unexpected(std::move(rpc)));
    }
    for (auto& [id, item] : sync) {
        const std::lock_guard lock(item->mutex);
        item->connection_error = message;
        item->ready = true;
        item->cv.notify_all();
    }
}

void Client::reader(std::stop_token stop) {
    for (;;) {
        if (stop.stop_requested()) return;
        std::optional<std::string> line = channel_.receive_line();
        if (!line) {
            if (!stop.stop_requested()) {
                finish_all(channel_.eof() ? "the backend connection was closed"
                                          : (channel_.error().empty() ? "connection lost" : channel_.error()));
            }
            return;
        }
        protocol::Message message = protocol::parse_message(*line);
        if (message.kind == protocol::Message::Kind::response) {
            std::shared_ptr<Pending> pending;
            std::shared_ptr<SyncPending> sync;
            {
                const std::lock_guard lock(mutex_);
                if (!message.response.id.empty()) {
                    if (const auto it = pending_.find(message.response.id); it != pending_.end()) {
                        pending = it->second;
                        pending_.erase(it);
                    } else if (const auto it = sync_.find(message.response.id); it != sync_.end()) {
                        sync = it->second;
                        sync_.erase(it);
                    }
                }
            }
            if (pending) {
                if (message.response.error) pending->done(std::unexpected(*message.response.error));
                else pending->done(std::move(message.response.result));
            } else if (sync) {
                const std::lock_guard lock(sync->mutex);
                sync->result = std::move(message.response.result);
                sync->error = message.response.error;
                sync->ready = true;
                sync->cv.notify_all();
            }
            continue;
        }
        if (message.kind != protocol::Message::Kind::notification) continue;
        if (message.notification.method == "event") {
            protocol::Event event = message.notification.params.get<protocol::Event>();
            if (event.kind == "interaction.requested") {
                if (callbacks_.interaction) {
                    callbacks_.interaction(event.data.get<protocol::InteractionRequest>());
                }
            } else if (event.kind == "interaction.closed") {
                if (callbacks_.interaction_closed) {
                    callbacks_.interaction_closed(event.data.value("interaction_id", ""));
                }
            } else if (callbacks_.event) {
                callbacks_.event(event);
            }
        }
    }
}

} // namespace dagent::client
