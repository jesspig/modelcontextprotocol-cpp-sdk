#include <detail/ResponseCache.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using mcp::JsonValue;
using mcp::detail::ResponseCache;

namespace {

void StorePublic(ResponseCache& cache, const std::string& key, int value) {
    cache.Store(key, ResponseCache::Scope::Public, JsonValue(value),
        std::chrono::hours(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

} // namespace

TEST(ResponseCacheTest, EvictsLeastRecentlyAccessedEntry) {
    ResponseCache cache(2);
    StorePublic(cache, "a", 1);
    StorePublic(cache, "b", 2);

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(cache.GetAny("a").has_value());
    StorePublic(cache, "c", 3);

    EXPECT_FALSE(cache.GetAny("b").has_value());
    EXPECT_EQ(cache.GetAny("a").value().GetInt(), 1);
    EXPECT_EQ(cache.GetAny("c").value().GetInt(), 3);
}

TEST(ResponseCacheTest, ReinsertedKeyCountsAsRecent) {
    ResponseCache cache(2);
    StorePublic(cache, "a", 1);
    StorePublic(cache, "b", 2);
    StorePublic(cache, "a", 10);
    StorePublic(cache, "c", 3);

    EXPECT_FALSE(cache.GetAny("b").has_value());
    EXPECT_EQ(cache.GetAny("a").value().GetInt(), 10);
    EXPECT_EQ(cache.GetAny("c").value().GetInt(), 3);
}

TEST(ResponseCacheTest, ScopesEvictIndependently) {
    ResponseCache cache(1);
    StorePublic(cache, "p", 1);
    cache.Store("r", ResponseCache::Scope::Private, JsonValue(2),
        std::chrono::hours(1));

    EXPECT_TRUE(cache.GetAny("p").has_value());
    EXPECT_TRUE(cache.GetAny("r").has_value());
}

TEST(ResponseCacheTest, ZeroCapacityStoresNothing) {
    ResponseCache cache(0);
    cache.Store("a", ResponseCache::Scope::Public, JsonValue(1),
        std::chrono::hours(1));
    EXPECT_FALSE(cache.GetAny("a").has_value());
}

TEST(ResponseCacheTest, ExpiredEntriesAreNotReturned) {
    ResponseCache cache(4);
    cache.Store("gone", ResponseCache::Scope::Public, JsonValue(1),
        std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_FALSE(cache.GetAny("gone").has_value());
}
