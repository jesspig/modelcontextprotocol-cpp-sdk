#pragma once

#include <mcp/http/EventStore.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

class FileEventStore : public EventStore {
public:
    explicit FileEventStore(std::filesystem::path storage_dir);

    uint64_t Append(std::string_view session_id,
                    std::string event_data) override;
    std::vector<std::pair<uint64_t, std::string>> GetEventsSince(
        std::string_view session_id,
        uint64_t last_event_id) const override;
    void Clear(std::string_view session_id) override;

private:
    std::filesystem::path SessionFilePath(std::string_view session_id) const;
    std::filesystem::path SessionLockPath(std::string_view session_id) const;

    std::filesystem::path storage_dir_;
};

} // namespace mcp
