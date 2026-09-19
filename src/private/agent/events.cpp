#include "agent/events.hpp"

namespace dagent::agent {

std::string_view to_string(TurnStatus status) {
    switch (status) {
    case TurnStatus::done: return "done";
    case TurnStatus::interrupted: return "interrupted";
    case TurnStatus::denied: return "denied";
    case TurnStatus::limit: return "limit";
    case TurnStatus::failed: return "failed";
    }
    return "unknown";
}

} // namespace dagent::agent
