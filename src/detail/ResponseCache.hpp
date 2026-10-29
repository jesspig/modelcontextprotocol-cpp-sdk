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
inline constexpr char kCacheAuthContextSeparator = '\x1F';

class ResponseCache {
public:
    enum class Scope { Public, Private };

    static constexpr std::chrono::milliseconds kMaxTtl = std::chrono::hours(24);
    static constexpr size_t kDefaultMaxEntriesPerScope = 512;

    explicit ResponseCache(
        size_t max_entries_per_scope = kDefaultMaxEntriesPerScope)
        : max_entries_per_scope_(max_entries_per_scope) {}

    void Store(std::string_view key, Scope scope, JsonValue value,
        std::chrono::milliseconds ttl,
        std::string_view auth_context = {})
    {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entries = Partition(scope);
        auto stored_key = scope == Scope::Private
            ? PrivateKey(auth_context, key)
            : std::string(key);
        auto [it, inserted] = entries.try_emplace(std::move(stored_key));
        it->second = Entry{std::move(value),
            now + (std::min)(ttl, kMaxTtl), now, now};
        EvictLocked(entries);
    }

    std::optional<JsonValue> GetAny(std::string_view key,
        std::optional<std::chrono::milliseconds> max_age = std::nullopt,
        std::string_view auth_context = {})
    {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto value = Lookup(public_entries_, key, max_age, now)) return value;
        return Lookup(private_entries_, PrivateKey(auth_context, key),
            max_age, now);
    }

    void Invalidate(std::string_view key) {
        std::lock_guard<std::mutex> lock(mutex_);
        public_entries_.erase(std::string(key));
        auto suffix = JoinWithSeparator(std::string_view(), key);
        for (auto it = private_entries_.begin();
             it != private_entries_.end();) {
            if (EndsWith(it->first, suffix)) {
                it = private_entries_.erase(it);
            } else {
                ++it;
            }
        }
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

    void ClearPrivate(std::string_view auth_context) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto prefix = JoinWithSeparator(auth_context, std::string_view());
        for (auto it = private_entries_.begin();
             it != private_entries_.end();) {
            if (StartsWith(it->first, prefix)) {
                it = private_entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    struct Entry {
        JsonValue value;
        std::chrono::steady_clock::time_point expires_at;
        std::chrono::steady_clock::time_point stored_at;
        std::chrono::steady_clock::time_point last_accessed;
    };

    static std::string JoinWithSeparator(
        std::string_view head, std::string_view tail)
    {
        std::string joined(head);
        joined.push_back(kCacheAuthContextSeparator);
        joined.append(tail);
        return joined;
    }

    static std::string PrivateKey(
        std::string_view auth_context, std::string_view key)
    {
        return JoinWithSeparator(auth_context, key);
    }

    static bool StartsWith(
        const std::string& value, std::string_view prefix)
    {
        return value.size() >= prefix.size() &&
            value.compare(0, prefix.size(), prefix) == 0;
    }

    static bool EndsWith(
        const std::string& value, std::string_view suffix)
    {
        return value.size() >= suffix.size() &&
            value.compare(value.size() - suffix.size(),
                suffix.size(), suffix) == 0;
    }

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
