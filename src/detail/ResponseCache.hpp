#pragma once
// ResponseCache.hpp — client-side response cache keyed by method (SEP-2549)
#include <mcp/JsonValue.hpp>

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace mcp { namespace detail {

// Client-side cache for list/read results declared with a ttlMs cache hint.
// Single-session clients treat "public" and "private" scopes identically, so
// entries are keyed by method only.
class ResponseCache {
public:
    void Store(std::string_view key, JsonValue value, std::chrono::milliseconds ttl) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[std::string(key)] = {
            std::move(value), std::chrono::steady_clock::now() + ttl};
    }

    // Returns the cached value, or nullopt when absent or expired.
    std::optional<JsonValue> Get(std::string_view key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(std::string(key));
        if (it == entries_.end()) return std::nullopt;
        if (std::chrono::steady_clock::now() >= it->second.expires_at) {
            entries_.erase(it);
            return std::nullopt;
        }
        return it->second.value;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

private:
    struct Entry {
        JsonValue value;
        std::chrono::steady_clock::time_point expires_at;
    };
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
};

}} // namespace mcp::detail
