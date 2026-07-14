#include <mcp/protocol/ICacheableResult.hpp>

namespace mcp {

ClientResponseCache::ClientResponseCache(size_t max_entries)
    : max_entries_(max_entries) {}

std::optional<nlohmann::json> ClientResponseCache::Get(const std::string& cache_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(cache_key);
    if (it == cache_.end()) return std::nullopt;

    if (std::chrono::steady_clock::now() >= it->second.expires_at) {
        cache_.erase(it);
        return std::nullopt;
    }

    return it->second.data;
}

void ClientResponseCache::Set(const std::string& cache_key, nlohmann::json data,
                               int64_t ttl_ms, const std::string& cache_scope) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (cache_.size() >= max_entries_) {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.expires_at < oldest->second.expires_at)
                oldest = it;
        }
        cache_.erase(oldest);
    }

    Entry entry;
    entry.data = std::move(data);
    entry.expires_at = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(ttl_ms);
    entry.cache_scope = cache_scope;
    cache_[cache_key] = std::move(entry);
}

void ClientResponseCache::Invalidate(const std::string& cache_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.erase(cache_key);
}

void ClientResponseCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

void ClientResponseCache::Prune() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (now >= it->second.expires_at)
            it = cache_.erase(it);
        else
            ++it;
    }
}

} // namespace mcp
