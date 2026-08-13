// EventStore.hpp - SSE event sequence store with resumption support

#pragma once

#include <mcp/JsonRpc.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp {

// ── EventStore — manages SSE event sequences and resumption tokens ──
// Used by StreamableHttpServerTransport to track pending events
// for each session and support resumption.
class EventStore {
public:
    EventStore() = default;

    // Append an event to the store. Returns the event ID.
    uint64_t Append(std::string_view session_id, std::string event_data);

    // Get events since a given event ID for a session.
    std::vector<std::pair<uint64_t, std::string>> GetEventsSince(
        std::string_view session_id, uint64_t last_event_id) const;

    // Clear events for a session.
    void Clear(std::string_view session_id);

    // Max events to keep per session (prevents unbounded growth).
    static constexpr size_t kMaxEventsPerSession = 1024;

private:
    struct StoredEvent {
        uint64_t id;
        std::string data;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<StoredEvent>> events_;
    uint64_t next_id_{1};
};

} // namespace mcp
