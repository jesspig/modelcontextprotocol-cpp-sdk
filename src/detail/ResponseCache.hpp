#pragma once
#include <mcp/JsonValue.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace mcp { namespace detail {

inline constexpr char kCacheScopePublic[] = "public";
inline constexpr char kCacheScopePrivate[] = "private";

class ResponseCache {
public:
    enum class Scope { Public, Private };

    static constexpr std::chrono::milliseconds kMaxTtl = std::chrono::hours(24);

    void Store(std::string_view key, Scope scope, JsonValue value,
        std::chrono::milliseconds ttl)
    {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        Partition(scope)[std::string(key)] = {
            std::move(value), now + (std::min)(ttl, kMaxTtl), now};
    }

    std::optional<JsonValue> GetAny(std::string_view key,
        std::optional<std::chrono::milliseconds> max_age = std::nullopt)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto value = Lookup(public_entries_, key, max_age)) return value;
        return Lookup(private_entries_, key, max_age);
    }

    void Invalidate(std::string_view key) {
        std::lock_guard<std::mutex> lock(mutex_);
        public_entries_.erase(std::string(key));
        private_entries_.erase(std::string(key));
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        public_entries_.clear();
        private_entries_.clear();
    }

    void ClearPrivate() {
        std::lock_guard<std::mutex> lock(mutex_);
        private_entries_.clear();
    }

private:
    struct Entry {
        JsonValue value;
        std::chrono::steady_clock::time_point expires_at;
        std::chrono::steady_clock::time_point stored_at;
    };

    static std::optional<JsonValue> Lookup(
        std::map<std::string, Entry>& entries, std::string_view key,
        std::optional<std::chrono::milliseconds> max_age)
    {
        auto it = entries.find(std::string(key));
        if (it == entries.end()) return std::nullopt;
        auto now = std::chrono::steady_clock::now();
        if (now >= it->second.expires_at ||
            (max_age && now - it->second.stored_at > *max_age)) {
            entries.erase(it);
            return std::nullopt;
        }
        return it->second.value;
    }

    std::map<std::string, Entry>& Partition(Scope scope) {
        return scope == Scope::Public ? public_entries_ : private_entries_;
    }

    std::mutex mutex_;
    std::map<std::string, Entry> public_entries_;
    std::map<std::string, Entry> private_entries_;
};

}} // namespace mcp::detail
