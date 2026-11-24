#include <mcp/storage/FileTokenCache.hpp>
#include <mcp/McpError.hpp>
#include <mcp/detail/AtomicJsonFile.hpp>

#include <fstream>
#include <gtest/gtest.h>
#include <mcp/test/McpTestInfo.hpp>
#include <cstdlib>
#include <filesystem>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

using namespace mcp;

namespace {

const ext::dpop::EcP256Jwk kValidDpopKey{
    "axfR8uEsQkf4vOblY6RA8ncDfYEt6zOg9KE5RdiYwpY",
    "T-NC4v4af5uO5-tKfA-eFivOM1drMV7Oy7ZAaDe_UfU",
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAE"};

void WriteRawCacheFile(const std::filesystem::path& path,
    const std::string& content)
{
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << content;
}

} // namespace

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

TEST_F(FileTokenCacheTest, DpopKeyValidationAcceptsP256AndRejectsMismatch) {
    EXPECT_TRUE(ext::dpop::ValidateKey(kValidDpopKey));

    ext::dpop::EcP256Jwk mismatched = kValidDpopKey;
    mismatched.d = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAg";
    EXPECT_FALSE(ext::dpop::ValidateKey(mismatched));

    ext::dpop::EcP256Jwk off_curve = kValidDpopKey;
    off_curve.x = std::string(43, 'A');
    EXPECT_FALSE(ext::dpop::ValidateKey(off_curve));

    ext::dpop::EcP256Jwk truncated = kValidDpopKey;
    truncated.d = "AA";
    EXPECT_FALSE(ext::dpop::ValidateKey(truncated));
}

TEST_F(FileTokenCacheTest, DpopKeyPersistsAcrossInstances) {
    {
        FileTokenCache cache(cache_path);
        TokenContainer tokens;
        tokens.access_token = "dpop-bound-token";
        cache.StoreTokens(tokens);
        cache.StoreDpopKey(kValidDpopKey);
    }
    FileTokenCache reopened(cache_path);
    auto key = reopened.GetDpopKey();
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(key->x, kValidDpopKey.x);
    EXPECT_EQ(key->y, kValidDpopKey.y);
    EXPECT_EQ(key->d.size(), kValidDpopKey.d.size());
    auto tokens = reopened.GetTokens();
    ASSERT_TRUE(tokens.has_value());
    EXPECT_EQ(tokens->access_token, "dpop-bound-token");
}

TEST_F(FileTokenCacheTest, OversizedIssuerAnchorRejectedWithoutClearingTokens) {
    FileTokenCache cache(cache_path);
    TokenContainer tokens;
    tokens.access_token = "anchor-bound-token";
    cache.StoreTokens(tokens);
    cache.StoreBoundIssuer("https://auth.example.com");

    auto anchor = cache_path;
    anchor += ".issuer";
    WriteRawCacheFile(anchor, std::string(8192, 'a'));

    FileTokenCache reopened(cache_path);
    EXPECT_FALSE(reopened.GetBoundIssuer().has_value());
    auto loaded = reopened.GetTokens();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->access_token, "anchor-bound-token");
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

TEST_F(FileTokenCacheTest, MismatchedDpopKeyRejectedAndOriginalKept) {
    const std::string original = std::string(
        "{\"access_token\":\"kept-token\",\"expires_at\":9999999999999,"
        "\"token_type\":\"Bearer\",\"dpop_key\":{\"x\":\"") +
        kValidDpopKey.x + "\",\"y\":\"" + kValidDpopKey.y +
        "\",\"d\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAg\"}}";
    WriteRawCacheFile(cache_path, original);

    FileTokenCache cache(cache_path);
    EXPECT_FALSE(cache.GetDpopKey().has_value());
    auto tokens = cache.GetTokens();
    ASSERT_TRUE(tokens.has_value());
    EXPECT_EQ(tokens->access_token, "kept-token");

    cache.StoreDpopKey(kValidDpopKey);
    FileTokenCache reopened(cache_path);
    EXPECT_FALSE(reopened.GetDpopKey().has_value());

    std::ifstream verify(cache_path, std::ios::binary);
    std::string after((std::istreambuf_iterator<char>(verify)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(after.find("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAg"),
        std::string::npos);
    EXPECT_EQ(after.find(kValidDpopKey.d), std::string::npos);
}

TEST_F(FileTokenCacheTest, SymlinkedIssuerAnchorRejected) {
    auto anchor = cache_path;
    anchor += ".issuer";
    auto target = cache_path;
    target += ".issuer-target";
    WriteRawCacheFile(target, "https://auth.example.com");
    std::error_code ec;
    std::filesystem::create_symlink(target, anchor, ec);
    ASSERT_FALSE(ec);

    FileTokenCache cache(cache_path);
    EXPECT_FALSE(cache.GetBoundIssuer().has_value());
    std::filesystem::remove(target, ec);
    std::filesystem::remove(anchor, ec);
}

TEST_F(FileTokenCacheTest, DirectoryIssuerAnchorRejected) {
    auto anchor = cache_path;
    anchor += ".issuer";
    std::error_code ec;
    std::filesystem::create_directory(anchor, ec);
    ASSERT_FALSE(ec);

    FileTokenCache cache(cache_path);
    EXPECT_FALSE(cache.GetBoundIssuer().has_value());
    std::filesystem::remove(anchor, ec);
}

TEST_F(FileTokenCacheTest, WorldReadableIssuerAnchorRejected) {
    FileTokenCache cache(cache_path);
    cache.StoreBoundIssuer("https://auth.example.com");
    auto anchor = cache_path;
    anchor += ".issuer";
    ASSERT_EQ(::chmod(anchor.c_str(), 0644), 0);
    EXPECT_FALSE(cache.GetBoundIssuer().has_value());
}
#endif

static std::string ReadAtomicBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

static int CountAtomicTemps(const std::filesystem::path& path) {
    auto parent = path.parent_path();
    auto prefix = path.filename().string() + ".tmp.";
    int count = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
        if (entry.path().filename().string().rfind(prefix, 0) == 0) ++count;
    }
    return count;
}

TEST_F(FileTokenCacheTest, AtomicWriteFailureStagesKeepOldOrCompleteNew) {
    const std::string old_content = R"({"version":"old"})";
    const std::string new_content = R"({"version":"new"})";
    const std::vector<detail::AtomicWriteStage> stages = {
        detail::AtomicWriteStage::Create,
        detail::AtomicWriteStage::Write,
        detail::AtomicWriteStage::Flush,
        detail::AtomicWriteStage::Rename,
        detail::AtomicWriteStage::ParentSync,
    };
    for (auto stage : stages) {
        {
            std::ofstream file(cache_path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(file.is_open());
            file << old_content;
        }
        detail::SetAtomicWriteFailureInjection(stage);
        bool ok = detail::WriteAtomic(cache_path, new_content);
        detail::SetAtomicWriteFailureInjection(detail::AtomicWriteStage::None);

        EXPECT_FALSE(ok);
        auto content = ReadAtomicBytes(cache_path);
        EXPECT_TRUE(content == old_content || content == new_content);
        if (stage == detail::AtomicWriteStage::ParentSync) {
            EXPECT_EQ(content, new_content);
        } else {
            EXPECT_EQ(content, old_content);
        }
        EXPECT_EQ(CountAtomicTemps(cache_path), 0);
    }
}

TEST_F(FileTokenCacheTest, AtomicSecureWriteFailureThrowsAndCleansTemp) {
    const std::string old_content = R"({"version":"old"})";
    {
        std::ofstream file(cache_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.is_open());
        file << old_content;
    }
    detail::SetAtomicWriteFailureInjection(detail::AtomicWriteStage::Rename);
    EXPECT_THROW((void)detail::WriteAtomicSecure(cache_path, R"({"version":"new"})"), McpError);
    detail::SetAtomicWriteFailureInjection(detail::AtomicWriteStage::None);
    EXPECT_EQ(ReadAtomicBytes(cache_path), old_content);
    EXPECT_EQ(CountAtomicTemps(cache_path), 0);
}

TEST_F(FileTokenCacheTest, AtomicWriteRejectsDirectoryTarget) {
    auto directory = cache_path.string() + ".dir";
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    EXPECT_FALSE(detail::WriteAtomic(std::filesystem::path(directory), std::string("payload")));
    std::error_code ec;
    std::filesystem::remove(directory, ec);
}

#ifndef _WIN32
TEST_F(FileTokenCacheTest, AtomicWriteRejectsSymlinkTarget) {
    auto victim = cache_path.string() + ".victim";
    {
        std::ofstream file(victim, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.is_open());
        file << "keep";
    }
    ASSERT_EQ(::symlink(victim.c_str(), cache_path.c_str()), 0);
    EXPECT_FALSE(detail::WriteAtomic(cache_path, "changed"));
    EXPECT_EQ(ReadAtomicBytes(victim), "keep");
    std::error_code ec;
    std::filesystem::remove(cache_path, ec);
    std::filesystem::remove(victim, ec);
}
#else
TEST_F(FileTokenCacheTest, AtomicWriteRejectsReparsePointTarget) {
    auto directory = cache_path.string() + ".dir";
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    auto link = cache_path.string() + ".link";
    std::wstring target(directory.begin(), directory.end());
    std::wstring link_name(link.begin(), link.end());
    if (!::CreateSymbolicLinkW(link_name.c_str(), target.c_str(),
                               SYMBOLIC_LINK_FLAG_DIRECTORY)) {
        std::wstring command = L"cmd /c mklink /J \"" + link_name +
            L"\" \"" + target + L"\"";
        if (::_wsystem(command.c_str()) != 0) {
            std::error_code ec;
            std::filesystem::remove(directory, ec);
            GTEST_SKIP() << "symbolic link and junction creation unavailable";
        }
    }
    EXPECT_FALSE(detail::WriteAtomic(std::filesystem::path(link), std::string("payload")));
    EXPECT_EQ(CountAtomicTemps(std::filesystem::path(link)), 0);
    std::error_code ec;
    std::filesystem::remove(link, ec);
    std::filesystem::remove(directory, ec);
}
#endif
