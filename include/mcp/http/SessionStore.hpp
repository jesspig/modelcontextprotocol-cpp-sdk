#pragma once

#include <cstdint>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace mcp {

struct SessionRecord {
    std::string protocol_version;
    int64_t created_at_ms{0};
};

class SessionStore {
public:
    virtual ~SessionStore() = default;

    virtual void Save(const std::string& session_id, const SessionRecord& record) = 0;
    virtual std::optional<SessionRecord> Load(const std::string& session_id) = 0;
    virtual void Remove(const std::string& session_id) = 0;
};

class InMemorySessionStore : public SessionStore {
public:
    void Save(const std::string& session_id, const SessionRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        records_[session_id] = record;
    }

    std::optional<SessionRecord> Load(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(session_id);
        if (it == records_.end()) return std::nullopt;
        return it->second;
    }

    void Remove(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.erase(session_id);
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionRecord> records_;
};

} // namespace mcp
