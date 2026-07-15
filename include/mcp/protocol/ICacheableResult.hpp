#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace mcp {

// ═══════════════════════════════════════════════════════════════════════
// ICacheableResult — results that can carry caching metadata (SEP-2549)
// ═══════════════════════════════════════════════════════════════════════
struct ICacheableResult {
    virtual ~ICacheableResult() = default;

    virtual std::optional<int64_t> TimeToLiveMs() const = 0;
    virtual std::optional<std::string> CacheScope() const = 0;
    virtual void SetCacheHint(int64_t ttl_ms, const std::string& scope) = 0;
};

// ═══════════════════════════════════════════════════════════════════════
// ClientResponseCache — simple in-memory cache (SEP-2549)
// ═══════════════════════════════════════════════════════════════════════
class ClientResponseCache {
public:
    struct Entry {
        nlohmann::json data;
        std::chrono::steady_clock::time_point expires_at;
        std::string cache_scope;
    };

    ClientResponseCache(size_t max_entries = 1024);

    std::optional<nlohmann::json> Get(const std::string& cache_key);
    void Set(const std::string& cache_key, nlohmann::json data,
             int64_t ttl_ms, const std::string& cache_scope = "private");
    void Invalidate(const std::string& cache_key);
    void Clear();
    void Prune();

    size_t Size() const { return cache_.size(); }

private:
    size_t max_entries_;
    std::unordered_map<std::string, Entry> cache_;
    std::mutex mutex_;
};

} // namespace mcp
