#pragma once

#include <mcp/transport/detail/Limits.hpp>

#include <chrono>
#include <cstdint>

#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace mcp {

struct SessionRecord {
    std::string protocol_version;
    int64_t created_at_ms{0};
    int64_t last_seen_ms{0};
};

class SessionStore {
public:
    virtual ~SessionStore() = default;

    virtual void Save(const std::string& session_id, const SessionRecord& record) = 0;
    virtual std::optional<SessionRecord> Load(const std::string& session_id) = 0;
    virtual void Remove(const std::string& session_id) = 0;

    virtual bool TrySave(const std::string& session_id, const SessionRecord& record) {
        Save(session_id, record);
        return true;
    }

    virtual size_t Size() const { return 0; }

    virtual size_t SweepExpired() { return 0; }
};

class InMemorySessionStore : public SessionStore {
public:
    void Save(const std::string& session_id, const SessionRecord& record) override {
        if (!TrySave(session_id, record))
            throw std::runtime_error("session store is full");
    }

    bool TrySave(const std::string& session_id, const SessionRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = NowMs();
        SweepExpiredLocked(now);
        auto it = records_.find(session_id);
        if (it == records_.end() && records_.size() >= detail::kMaxHttpSessions)
            return false;
        SessionRecord stored = record;
        if (stored.last_seen_ms == 0)
            stored.last_seen_ms = now;
        records_[session_id] = stored;
        return true;
    }

    std::optional<SessionRecord> Load(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = NowMs();
        auto it = records_.find(session_id);
        if (it == records_.end()) return std::nullopt;
        if (IsExpired(it->second, now)) {
            records_.erase(it);
            return std::nullopt;
        }
        it->second.last_seen_ms = now;
        return it->second;
    }

    void Remove(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.erase(session_id);
    }

    size_t Size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_.size();
    }

    size_t SweepExpired() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return SweepExpiredLocked(NowMs());
    }

private:
    static int64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static int64_t EffectiveLastSeen(const SessionRecord& record) {
        if (record.last_seen_ms != 0) return record.last_seen_ms;
        return record.created_at_ms;
    }

    static bool IsExpired(const SessionRecord& record, int64_t now) {
        auto last = EffectiveLastSeen(record);
        if (last == 0) return false;
        return now - last > detail::kSessionIdleTimeoutMs;
    }

    size_t SweepExpiredLocked(int64_t now) {
        size_t removed = 0;
        for (auto it = records_.begin(); it != records_.end();) {
            if (IsExpired(it->second, now)) {
                it = records_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionRecord> records_;
};

} // namespace mcp
