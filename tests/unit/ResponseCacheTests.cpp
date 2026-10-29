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

TEST(ResponseCacheTest, PrivateEntriesIsolatedByAuthContext) {
    ResponseCache cache(4);
    cache.Store("res", ResponseCache::Scope::Private, JsonValue(1),
        std::chrono::hours(1), "user-a");
    cache.Store("res", ResponseCache::Scope::Private, JsonValue(2),
        std::chrono::hours(1), "user-b");

    auto a = cache.GetAny("res", std::nullopt, "user-a");
    auto b = cache.GetAny("res", std::nullopt, "user-b");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->GetInt(), 1);
    EXPECT_EQ(b->GetInt(), 2);
    EXPECT_FALSE(cache.GetAny("res").has_value());
}

TEST(ResponseCacheTest, PublicEntriesSharedAcrossAuthContexts) {
    ResponseCache cache(4);
    cache.Store("shared", ResponseCache::Scope::Public, JsonValue(7),
        std::chrono::hours(1), "user-a");

    auto b = cache.GetAny("shared", std::nullopt, "user-b");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->GetInt(), 7);

    cache.Store("shared", ResponseCache::Scope::Public, JsonValue(8),
        std::chrono::hours(1), "user-b");
    auto a = cache.GetAny("shared", std::nullopt, "user-a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->GetInt(), 8);
}

TEST(ResponseCacheTest, ClearPrivateOnlyClearsGivenAuthContext) {
    ResponseCache cache(8);
    cache.Store("priv", ResponseCache::Scope::Private, JsonValue(1),
        std::chrono::hours(1), "user-a");
    cache.Store("priv", ResponseCache::Scope::Private, JsonValue(2),
        std::chrono::hours(1), "user-b");
    cache.Store("pub", ResponseCache::Scope::Public, JsonValue(3),
        std::chrono::hours(1));

    cache.ClearPrivate("user-a");

    EXPECT_FALSE(
        cache.GetAny("priv", std::nullopt, "user-a").has_value());
    EXPECT_TRUE(cache.GetAny("priv", std::nullopt, "user-b").has_value());
    EXPECT_TRUE(cache.GetAny("pub").has_value());

    cache.ClearPrivate();
    EXPECT_FALSE(
        cache.GetAny("priv", std::nullopt, "user-b").has_value());
    EXPECT_TRUE(cache.GetAny("pub").has_value());
}

TEST(ResponseCacheTest, InvalidateRemovesKeyAcrossAuthContexts) {
    ResponseCache cache(8);
    cache.Store("res", ResponseCache::Scope::Private, JsonValue(1),
        std::chrono::hours(1), "user-a");
    cache.Store("res", ResponseCache::Scope::Private, JsonValue(2),
        std::chrono::hours(1), "user-b");
    cache.Store("res", ResponseCache::Scope::Public, JsonValue(3),
        std::chrono::hours(1));
    cache.Store("other", ResponseCache::Scope::Private, JsonValue(4),
        std::chrono::hours(1), "user-a");

    cache.Invalidate("res");

    EXPECT_FALSE(cache.GetAny("res", std::nullopt, "user-a").has_value());
    EXPECT_FALSE(cache.GetAny("res", std::nullopt, "user-b").has_value());
    EXPECT_FALSE(cache.GetAny("res").has_value());
    EXPECT_TRUE(cache.GetAny("other", std::nullopt, "user-a").has_value());
}

TEST(ResponseCacheTest, DefaultAuthContextRoundTrips) {
    ResponseCache cache(4);
    cache.Store("k", ResponseCache::Scope::Private, JsonValue(5),
        std::chrono::hours(1));

    auto stored = cache.GetAny("k");
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->GetInt(), 5);

    auto aged = cache.GetAny("k", std::chrono::hours(1));
    ASSERT_TRUE(aged.has_value());
    EXPECT_EQ(aged->GetInt(), 5);

    EXPECT_FALSE(cache.GetAny("k", std::nullopt, "user-a").has_value());
}
