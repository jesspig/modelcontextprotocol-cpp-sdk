// FileTokenCacheTests — unit tests for FileTokenCache persistence

#include <mcp/storage/FileTokenCache.hpp>

#include <fstream>
#include <gtest/gtest.h>
#include <filesystem>

using namespace mcp;

class FileTokenCacheTest : public ::testing::Test {
protected:
    std::filesystem::path cache_path;

    void SetUp() override {
        auto temp_dir = std::filesystem::temp_directory_path();
        cache_path = temp_dir / ("mcp_test_tokens_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) +
            ".json");
        RemoveFiles();
    }

    void TearDown() override {
        RemoveFiles();
    }

private:
    void RemoveFiles() {
        std::error_code ec;
        std::filesystem::remove(cache_path, ec);
        std::filesystem::remove(cache_path.string() + ".token_response", ec);
        std::filesystem::remove(cache_path.string() + ".client_registration", ec);
    }
};

// ── Create cache ──
TEST_F(FileTokenCacheTest, CreateCache) {
    FileTokenCache cache(cache_path);
    TokenContainer tokens;
    tokens.access_token = "test";
    cache.StoreTokens(tokens);
    EXPECT_TRUE(std::filesystem::exists(cache_path));
}

// ── Store and retrieve tokens ──
TEST_F(FileTokenCacheTest, StoreAndRetrieveTokens) {
    FileTokenCache cache(cache_path);

    TokenContainer tokens;
    tokens.access_token = "test_access_token";
    tokens.refresh_token = "test_refresh_token";
    tokens.scopes = {"read", "write"};
    tokens.expires_at = 1234567890000;
    tokens.token_type = "Bearer";

    cache.StoreTokens(tokens);

    auto result = cache.GetTokens();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->access_token, "test_access_token");
    EXPECT_EQ(result->refresh_token, "test_refresh_token");
    ASSERT_EQ(result->scopes.size(), 2U);
    EXPECT_EQ(result->scopes[0], "read");
    EXPECT_EQ(result->scopes[1], "write");
    EXPECT_EQ(result->expires_at, 1234567890000);
    EXPECT_EQ(result->token_type, "Bearer");
}

// ── Clear tokens ──
TEST_F(FileTokenCacheTest, ClearTokens) {
    FileTokenCache cache(cache_path);

    TokenContainer tokens;
    tokens.access_token = "test_access_token";
    cache.StoreTokens(tokens);
    cache.ClearTokens();

    auto result = cache.GetTokens();
    EXPECT_FALSE(result.has_value());
}

// ── Persist across instances ──
TEST_F(FileTokenCacheTest, PersistAcrossInstances) {
    {
        FileTokenCache cache_a(cache_path);

        TokenContainer tokens;
        tokens.access_token = "persisted_token";
        tokens.refresh_token = "persisted_refresh";
        tokens.scopes = {"read"};
        tokens.expires_at = 1234567890000;
        tokens.token_type = "Bearer";

        cache_a.StoreTokens(tokens);
    } // cache_a destroyed

    FileTokenCache cache_b(cache_path);

    auto result = cache_b.GetTokens();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->access_token, "persisted_token");
    EXPECT_EQ(result->refresh_token, "persisted_refresh");
    ASSERT_EQ(result->scopes.size(), 1U);
    EXPECT_EQ(result->scopes[0], "read");
    EXPECT_EQ(result->expires_at, 1234567890000);
    EXPECT_EQ(result->token_type, "Bearer");
}

// ── Corrupt file handling ──
// A corrupt cache file must not throw in the constructor's Load() and
// yields no tokens.
TEST_F(FileTokenCacheTest, CorruptFileHandling) {
    {
        std::ofstream ofs(cache_path);
        ASSERT_TRUE(ofs.is_open());
        ofs << "this is not valid json";
    }

    FileTokenCache cache(cache_path);

    EXPECT_FALSE(cache.GetTokens().has_value());
}
