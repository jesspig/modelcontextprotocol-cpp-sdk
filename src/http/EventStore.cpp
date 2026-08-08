// EventStore.cpp - Event sequence store implementation

#include <mcp/http/EventStore.hpp>

#include <mutex>

namespace mcp {

uint64_t EventStore::Append(
    std::string_view session_id, std::string event_data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto id = next_id_++;
    auto& events = events_[std::string(session_id)];
    events.push_back({id, std::move(event_data)});

    // Trim excess events for this session only
    if (events.size() > kMaxEventsPerSession) {
        events.erase(
            events.begin(),
            events.begin() + (events.size() - kMaxEventsPerSession));
    }
    return id;
}

std::vector<std::string> EventStore::GetEventsSince(
    std::string_view session_id, uint64_t last_event_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    auto it = events_.find(std::string(session_id));
    if (it == events_.end()) return result;
    for (const auto& ev : it->second) {
        if (ev.id > last_event_id) {
            result.push_back(ev.data);
        }
    }
    return result;
}

void EventStore::Clear(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.erase(std::string(session_id));
}

bool EventStore::HasEvents(std::string_view session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.find(std::string(session_id)) != events_.end();
}

} // namespace mcp
