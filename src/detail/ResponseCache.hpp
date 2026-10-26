#pragma once
#include <mcp/JsonValue.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
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
    static constexpr size_t kDefaultMaxEntriesPerScope = 512;

    explicit ResponseCache(
        size_t max_entries_per_scope = kDefaultMaxEntriesPerScope)
        : max_entries_per_scope_(max_entries_per_scope) {}

    void Store(std::string_view key, Scope scope, JsonValue value,
        std::chrono::milliseconds ttl)
    {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entries = Partition(scope);
        auto [it, inserted] = entries.try_emplace(std::string(key));
        it->second = Entry{std::move(value),
            now + (std::min)(ttl, kMaxTtl), now, now};
        EvictLocked(entries);
    }

    std::optional<JsonValue> GetAny(std::string_view key,
        std::optional<std::chrono::milliseconds> max_age = std::nullopt)
    {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto value = Lookup(public_entries_, key, max_age, now)) return value;
        return Lookup(private_entries_, key, max_age, now);
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
        std::chrono::steady_clock::time_point last_accessed;
    };

    static std::optional<JsonValue> Lookup(
        std::map<std::string, Entry>& entries, std::string_view key,
        std::optional<std::chrono::milliseconds> max_age,
        std::chrono::steady_clock::time_point now)
    {
        auto it = entries.find(std::string(key));
        if (it == entries.end()) return std::nullopt;
        if (now >= it->second.expires_at ||
            (max_age && now - it->second.stored_at > *max_age)) {
            entries.erase(it);
            return std::nullopt;
        }
        it->second.last_accessed = now;
        return it->second.value;
    }

    void EvictLocked(std::map<std::string, Entry>& entries) {
        while (entries.size() > max_entries_per_scope_) {
            auto victim = entries.begin();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->second.last_accessed < victim->second.last_accessed)
                    victim = it;
            }
            entries.erase(victim);
        }
    }

    std::map<std::string, Entry>& Partition(Scope scope) {
        return scope == Scope::Public ? public_entries_ : private_entries_;
    }

    size_t max_entries_per_scope_;
    std::mutex mutex_;
    std::map<std::string, Entry> public_entries_;
    std::map<std::string, Entry> private_entries_;
};

}} // namespace mcp::detail
