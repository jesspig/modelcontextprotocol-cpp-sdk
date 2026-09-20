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

class EventStore {
public:
    EventStore() = default;
    virtual ~EventStore() = default;

    virtual uint64_t Append(std::string_view session_id, std::string event_data);

    virtual std::vector<std::pair<uint64_t, std::string>> GetEventsSince(
        std::string_view session_id, uint64_t last_event_id) const;

    virtual void Clear(std::string_view session_id);

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
