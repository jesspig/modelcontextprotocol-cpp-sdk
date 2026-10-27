#include <mcp/storage/FileTokenCache.hpp>

#include <fstream>
#include <gtest/gtest.h>
#include <mcp/test/McpTestInfo.hpp>
#include <filesystem>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace mcp;

class FileTokenCacheTest : public ::testing::Test {
protected:
    std::filesystem::path cache_path;

    void SetUp() override {
        auto temp_dir = std::filesystem::temp_directory_path();
        cache_path = temp_dir / ("mcp_test_tokens_" +
            std::string(mcp::test::CurrentTestName()) +
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
        auto anchor = cache_path;
        anchor += ".issuer";
        std::filesystem::remove(anchor, ec);
    }
};

TEST_F(FileTokenCacheTest, CreateCache) {
    FileTokenCache cache(cache_path);
    TokenContainer tokens;
    tokens.access_token = "test";
    cache.StoreTokens(tokens);
    EXPECT_TRUE(std::filesystem::exists(cache_path));
}

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

TEST_F(FileTokenCacheTest, ClearTokens) {
    FileTokenCache cache(cache_path);

    TokenContainer tokens;
    tokens.access_token = "test_access_token";
    cache.StoreTokens(tokens);
    cache.ClearTokens();

    auto result = cache.GetTokens();
    EXPECT_FALSE(result.has_value());
}

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
    }

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

TEST_F(FileTokenCacheTest, CorruptFileHandling) {
    {
        std::ofstream ofs(cache_path);
        ASSERT_TRUE(ofs.is_open());
        ofs << "this is not valid json";
    }

    FileTokenCache cache(cache_path);

    EXPECT_FALSE(cache.GetTokens().has_value());
}

TEST_F(FileTokenCacheTest, BoundIssuerRoundTrip) {
    FileTokenCache cache(cache_path);
    EXPECT_FALSE(cache.GetBoundIssuer().has_value());
    cache.StoreBoundIssuer("https://auth.example.com");
    auto anchor = cache.GetBoundIssuer();
    ASSERT_TRUE(anchor.has_value());
    EXPECT_EQ(*anchor, "https://auth.example.com");

    FileTokenCache reopened(cache_path);
    auto persisted = reopened.GetBoundIssuer();
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(*persisted, "https://auth.example.com");
}

TEST_F(FileTokenCacheTest, BoundIssuerSurvivesTokenClear) {
    FileTokenCache cache(cache_path);
    TokenContainer tokens;
    tokens.access_token = "test";
    cache.StoreTokens(tokens);
    cache.StoreBoundIssuer("https://auth.example.com");
    cache.ClearTokens();
    auto anchor = cache.GetBoundIssuer();
    ASSERT_TRUE(anchor.has_value());
    EXPECT_EQ(*anchor, "https://auth.example.com");
}

#ifndef _WIN32
TEST_F(FileTokenCacheTest, TokenFileCreatedWithOwnerOnlyPermissions) {
    FileTokenCache cache(cache_path);
    TokenContainer tokens;
    tokens.access_token = "secret-access";
    tokens.refresh_token = "secret-refresh";
    cache.StoreTokens(tokens);

    struct stat info {};
    ASSERT_EQ(::stat(cache_path.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600);
}

TEST_F(FileTokenCacheTest, IssuerAnchorCreatedWithOwnerOnlyPermissions) {
    FileTokenCache cache(cache_path);
    cache.StoreBoundIssuer("https://auth.example.com");

    auto anchor = cache_path;
    anchor += ".issuer";
    struct stat info {};
    ASSERT_EQ(::stat(anchor.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600);
}

TEST_F(FileTokenCacheTest, WorldReadableTokenFileRejectedOnLoad) {
    {
        std::ofstream ofs(cache_path);
        ASSERT_TRUE(ofs.is_open());
        ofs << "{\"access_token\":\"leaked\",\"expires_at\":9999999999999}";
    }
    ASSERT_EQ(::chmod(cache_path.c_str(), 0644), 0);

    FileTokenCache cache(cache_path);
    EXPECT_FALSE(cache.GetTokens().has_value());
}

TEST_F(FileTokenCacheTest, NoTemporaryFilesLeftBehind) {
    FileTokenCache cache(cache_path);
    TokenContainer tokens;
    tokens.access_token = "secret";
    cache.StoreTokens(tokens);

    auto parent = cache_path.parent_path();
    int leftovers = 0;
    for (const auto& entry : std::filesystem::directory_iterator(parent)) {
        auto name = entry.path().filename().string();
        if (name.find("mcp_test_tokens_" + std::string(mcp::test::CurrentTestName())) == 0 &&
            name.find(".tmp.") != std::string::npos) {
            ++leftovers;
        }
    }
    EXPECT_EQ(leftovers, 0);
}
#endif
