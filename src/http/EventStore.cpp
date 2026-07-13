#include <mcp/http/EventStore.hpp>

#include <algorithm>
#include <mutex>

namespace mcp {

uint64_t EventStore::Append(
    std::string_view session_id, std::string event_data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto id = next_id_++;
    events_.push_back({id, std::string(session_id), std::move(event_data)});

    // Trim excess events
    if (events_.size() > kMaxEventsPerSession) {
        events_.erase(
            events_.begin(),
            events_.begin() + (events_.size() - kMaxEventsPerSession));
    }
    return id;
}

std::vector<std::string> EventStore::GetEventsSince(
    std::string_view session_id, uint64_t last_event_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (const auto& ev : events_) {
        if (ev.session_id == session_id && ev.id > last_event_id) {
            result.push_back(ev.data);
        }
    }
    return result;
}

void EventStore::Clear(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.erase(
        std::remove_if(events_.begin(), events_.end(),
            [&](const StoredEvent& ev) {
                return ev.session_id == session_id;
            }),
        events_.end());
}

bool EventStore::HasEvents(std::string_view session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(events_.begin(), events_.end(),
        [&](const StoredEvent& ev) {
            return ev.session_id == session_id;
        });
}

} // namespace mcp
