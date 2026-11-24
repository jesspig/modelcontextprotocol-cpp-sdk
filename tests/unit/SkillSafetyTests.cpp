#include <mcp/client/McpClient.hpp>
#include <mcp/client/SkillVerify.hpp>
#include <mcp/detail/sha256.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>

#include <gtest/gtest.h>
#include <mcp/test/McpTimeout.hpp>

#include <atomic>
#include <filesystem>
#include <future>
#include <string>
#include <string_view>
#include <vector>

using namespace mcp;

namespace {

std::string TestHex(std::string_view raw) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0f]);
    }
    return out;
}

std::string TestDigest(std::string_view bytes) {
    return "sha256:" + TestHex(detail::Sha256::Hash(bytes));
}

std::string TestBase64Encode(std::string_view raw) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((raw.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < raw.size()) {
        const uint32_t n = (static_cast<uint32_t>(static_cast<unsigned char>(raw[i])) << 16)
            | (static_cast<uint32_t>(static_cast<unsigned char>(raw[i + 1])) << 8)
            | static_cast<uint32_t>(static_cast<unsigned char>(raw[i + 2]));
        out.push_back(kAlphabet[(n >> 18) & 0x3f]);
        out.push_back(kAlphabet[(n >> 12) & 0x3f]);
        out.push_back(kAlphabet[(n >> 6) & 0x3f]);
        out.push_back(kAlphabet[n & 0x3f]);
        i += 3;
    }
    if (i + 1 == raw.size()) {
        const uint32_t n = static_cast<uint32_t>(static_cast<unsigned char>(raw[i])) << 16;
        out.push_back(kAlphabet[(n >> 18) & 0x3f]);
        out.push_back(kAlphabet[(n >> 12) & 0x3f]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == raw.size()) {
        const uint32_t n = (static_cast<uint32_t>(static_cast<unsigned char>(raw[i])) << 16)
            | (static_cast<uint32_t>(static_cast<unsigned char>(raw[i + 1])) << 8);
        out.push_back(kAlphabet[(n >> 18) & 0x3f]);
        out.push_back(kAlphabet[(n >> 12) & 0x3f]);
        out.push_back(kAlphabet[(n >> 6) & 0x3f]);
        out.push_back('=');
    }
    return out;
}

JsonValue TestSkillValue(const std::string& uri, const std::string& name,
    const std::vector<SkillResource>& resources)
{
    JsonValue skill(JsonValue::object_tag);
    skill["uri"] = JsonValue(uri);
    JsonValue frontmatter(JsonValue::object_tag);
    frontmatter["name"] = JsonValue(name);
    skill["frontmatter"] = std::move(frontmatter);
    JsonValue arr(JsonValue::array_tag);
    for (const auto& r : resources) {
        JsonValue entry(JsonValue::object_tag);
        entry["uri"] = JsonValue(r.uri);
        entry["digest"] = JsonValue(r.digest);
        entry["size"] = JsonValue(r.size);
        arr.PushBack(std::move(entry));
    }
    skill["resources"] = std::move(arr);
    return skill;
}

JsonValue TestSkillGetResult(JsonValue skill) {
    JsonValue result(JsonValue::object_tag);
    result["skill"] = std::move(skill);
    result["resultType"] = JsonValue("complete");
    return result;
}

JsonValue TestReadResourceResult(const std::string& uri, const std::string& field,
    const std::string& value)
{
    JsonValue result(JsonValue::object_tag);
    JsonValue arr(JsonValue::array_tag);
    JsonValue content(JsonValue::object_tag);
    content["uri"] = JsonValue(uri);
    content[field] = JsonValue(value);
    arr.PushBack(std::move(content));
    result["contents"] = std::move(arr);
    return result;
}

} // namespace

TEST(SkillSafetyTest, RawSizeAndDigestComputedBeforeDecode) {
    const std::string encoded = "c2tpbGwgYm9keQ==";
    std::string error;
    SkillBlob blob = SkillBlob::FromEncoded(encoded, error);
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(blob.IsDecoded());
    EXPECT_EQ(blob.Metrics().raw_size, encoded.size());
    EXPECT_EQ(TestHex(std::string(reinterpret_cast<const char*>(
        blob.Metrics().raw_digest.data()), blob.Metrics().raw_digest.size())),
        TestHex(detail::Sha256::Hash(encoded)));
    EXPECT_EQ(blob.Metrics().decoded_size, 0u);
    ASSERT_TRUE(blob.Decode(error));
    EXPECT_EQ(blob.Metrics().decoded_size, 10u);
    EXPECT_EQ(blob.Decoded(), "skill body");
}

TEST(SkillSafetyTest, StrictBase64TruncatedAndTrailingRejected) {
    std::string error;
    auto truncated = SkillBlob::FromEncoded("c2tpbGwgYm9keQ", error);
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(truncated.Decode(error));
    EXPECT_FALSE(error.empty());
    error.clear();
    auto trailing = SkillBlob::FromEncoded("c2tpbGwgYm9keQ==A", error);
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(trailing.Decode(error));
    EXPECT_FALSE(error.empty());
}

TEST(SkillSafetyTest, StrictBase64AlphabetAndCanonicalRejected) {
    std::string error;
    auto invalid = SkillBlob::FromEncoded("!!!!", error);
    EXPECT_FALSE(invalid.Decode(error));
    EXPECT_NE(error.find("alphabet"), std::string::npos);
    error.clear();
    auto noncanonical = SkillBlob::FromEncoded("c2tpbGwgYm9keR==", error);
    EXPECT_FALSE(noncanonical.Decode(error));
    EXPECT_NE(error.find("canonical"), std::string::npos);
}

TEST(SkillSafetyTest, StrictBase64PaddingRoundTrips) {
    struct Vector { const char* encoded; const char* decoded; };
    const Vector vectors[] = {
        {"YQ==", "a"}, {"YWI=", "ab"}, {"YWJj", "abc"}, {"YWJjZA==", "abcd"}};
    for (const auto& vector : vectors) {
        std::string error;
        auto blob = SkillBlob::FromEncoded(vector.encoded, error);
        ASSERT_TRUE(error.empty());
        ASSERT_TRUE(blob.Decode(error)) << vector.encoded << ": " << error;
        EXPECT_EQ(blob.Decoded(), vector.decoded);
        EXPECT_EQ(blob.Metrics().decoded_size, std::string(vector.decoded).size());
    }
}

TEST(SkillSafetyTest, EncodedAndDecodedByteBoundariesRejected) {
    std::string error;
    const std::string too_long(kMaxSkillEncodedChars + 1, 'A');
    auto oversized = SkillBlob::FromEncoded(too_long, error);
    EXPECT_NE(error.find("bound"), std::string::npos);
    error.clear();
    const std::string expands(kMaxSkillEncodedChars, 'A');
    auto expanded = SkillBlob::FromEncoded(expands.substr(0, expands.size() - 1) + "=", error);
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(expanded.Decode(error));
    EXPECT_NE(error.find("budget"), std::string::npos);
}

TEST(SkillSafetyTest, RealYamlFrontmatterParsedAndComparedToBytes) {
    JsonValue parsed;
    std::string error;
    ASSERT_TRUE(ParseFrontmatter("name: code-review\ndescription: review code\n", parsed, error));
    ASSERT_TRUE(parsed.IsObject());
    auto* name = parsed.Find("name");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->GetString(), "code-review");
    auto* description = parsed.Find("description");
    ASSERT_NE(description, nullptr);
    EXPECT_EQ(description->GetString(), "review code");

    Skill advertised;
    advertised.uri = "skill://code-review/SKILL.md";
    advertised.frontmatter = JsonValue(JsonValue::object_tag);
    advertised.frontmatter["name"] = JsonValue("code-review");
    const std::string document = "---\nname: code-review\n---\nbody\n";
    EXPECT_TRUE(VerifyFrontmatterBytes(advertised, document, error));
    advertised.frontmatter["name"] = JsonValue("other");
    EXPECT_FALSE(VerifyFrontmatterBytes(advertised, document, error));
}

TEST(SkillSafetyTest, YamlDuplicateKeyAliasTagMergeAndImplicitRejected) {
    JsonValue parsed;
    std::string error;
    EXPECT_FALSE(ParseFrontmatter("name: a\nname: b\n", parsed, error));
    EXPECT_NE(error.find("duplicate"), std::string::npos);
    error.clear();
    EXPECT_FALSE(ParseFrontmatter("name: &a code-review\ndescription: *a\n", parsed, error));
    EXPECT_NE(error.find("alias"), std::string::npos);
    error.clear();
    EXPECT_FALSE(ParseFrontmatter("name: !!str code-review\n", parsed, error));
    EXPECT_NE(error.find("tag"), std::string::npos);
    error.clear();
    EXPECT_FALSE(ParseFrontmatter("<<: {name: a}\n", parsed, error));
    EXPECT_NE(error.find("merge"), std::string::npos);
    error.clear();
    EXPECT_FALSE(ParseFrontmatter("name: 123\n", parsed, error));
    EXPECT_NE(error.find("implicit"), std::string::npos);
}

TEST(SkillSafetyTest, YamlDepthNodeScalarAndDocumentBudgetsRejected) {
    JsonValue parsed;
    std::string error;
    std::string deep;
    for (int i = 0; i < 70; ++i) deep += std::string(static_cast<size_t>(i) * 2, ' ') + "a:\n";
    EXPECT_FALSE(ParseFrontmatter(deep, parsed, error));
    EXPECT_NE(error.find("depth"), std::string::npos);
    error.clear();
    std::string wide;
    for (int i = 0; i < 10001; ++i)
        wide += "k" + std::to_string(i) + ": v\n";
    EXPECT_FALSE(ParseFrontmatter(wide, parsed, error));
    EXPECT_NE(error.find("node"), std::string::npos);
    error.clear();
    const std::string scalar = "name: \"" + std::string(kMaxYamlScalarBytes + 1, 'a') + "\"\n";
    EXPECT_FALSE(ParseFrontmatter(scalar, parsed, error));
    EXPECT_NE(error.find("scalar"), std::string::npos);
    error.clear();
    EXPECT_FALSE(ParseFrontmatter("a: b\n---\nc: d\n", parsed, error));
    EXPECT_FALSE(error.empty());
}

TEST(SkillSafetyTest, LazyFrontmatterAndBodyExpandIndependently) {
    const std::string document = "---\nname: code-review\n---\nbody text\n";
    std::string error;
    const SkillBlob source = SkillBlob::FromEncoded(TestBase64Encode(document), error);
    SkillBlob first = source;
    JsonValue frontmatter;
    ASSERT_TRUE(first.Frontmatter(frontmatter, error));
    EXPECT_FALSE(first.HasBody());
    std::string body;
    ASSERT_TRUE(first.Body(body, error));
    EXPECT_EQ(body, "body text\n");

    SkillBlob second = source;
    ASSERT_TRUE(second.Body(body, error));
    EXPECT_FALSE(second.HasFrontmatter());
    ASSERT_TRUE(second.Frontmatter(frontmatter, error));
    EXPECT_EQ(frontmatter.Find("name")->GetString(), "code-review");
}

TEST(SkillSafetyTest, RouteUsesVersionAndDeclaredCapability) {
    ServerCapabilities capabilities;
    EXPECT_EQ(ResolveSkillRoute("2025-11-25", capabilities), SkillRoute::Legacy);
    EXPECT_EQ(ResolveSkillRoute("2026-07-28", capabilities), SkillRoute::Modern);
    capabilities.extensions.emplace();
    EXPECT_EQ(ResolveSkillRoute("2026-07-28", capabilities), SkillRoute::Legacy);
    (*capabilities.extensions)[std::string(kSkillsExtensionId)] = JsonValue(JsonValue::object_tag);
    EXPECT_EQ(ResolveSkillRoute("2026-07-28", capabilities), SkillRoute::Modern);
}

TEST(SkillSafetyTest, LocalPathRejectsAbsoluteTraversalAndSymlinkEscape) {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(".tmp") / "skill-safety-root";
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_FALSE(ec);
    std::string resolved;
    std::string error;
    EXPECT_FALSE(ResolveSkillLocalPath(root.u8string(), "/etc/passwd", resolved, error));
    EXPECT_NE(error.find("absolute"), std::string::npos);
    error.clear();
    EXPECT_FALSE(ResolveSkillLocalPath(root.u8string(), "../secrets.txt", resolved, error));
    EXPECT_NE(error.find("escapes"), std::string::npos);
    error.clear();
    EXPECT_TRUE(ResolveSkillLocalPath(root.u8string(), "notes/SKILL.md", resolved, error));
    EXPECT_TRUE(error.empty());

    const fs::path outside = root.parent_path() / "skill-safety-outside";
    fs::create_directories(outside, ec);
    fs::create_directory_symlink(outside, root / "escape", ec);
    if (ec) GTEST_SKIP() << "symlink creation not permitted on this host";
    error.clear();
    EXPECT_FALSE(ResolveSkillLocalPath(root.u8string(), "escape/SKILL.md", resolved, error));
    EXPECT_NE(error.find("symbolic"), std::string::npos);
}

TEST(SkillSafetyTest, ClientDecodesBlobBeforeSizeAndDigestVerification) {
    const std::string body = "skill body";
    const std::string uri = "skill://code-review/SKILL.md";
    auto pair = InMemoryTransport::CreatePair();
    auto server = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::vector<SkillResource> manifest{
        SkillResource{uri, TestDigest(body), static_cast<int64_t>(body.size())}};
    server->SetRequestHandler(methods::kGetSkill,
        [&](const JsonRpcRequest&, std::promise<JsonValue> p) {
            p.set_value(TestSkillGetResult(TestSkillValue(uri, "code-review", manifest)));
        });
    server->SetRequestHandler(methods::kReadResource,
        [&](const JsonRpcRequest&, std::promise<JsonValue> p) {
            p.set_value(TestReadResourceResult(uri, "blob", TestBase64Encode(body)));
        });
    server->Start();
    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.enable_skills_extension = true;
    auto client = McpClient::Create(std::move(pair.client), opts);

    const auto result = client->ReadSkill(uri);
    EXPECT_TRUE(result.diagnostics.empty());
    ASSERT_EQ(result.files.size(), 1u);
    EXPECT_EQ(result.files[0].bytes, body);
    client->Close();
    server->Close();
}

TEST(SkillSafetyTest, ClientRejectsMalformedBlobWithoutPartialContent) {
    const std::string uri = "skill://code-review/SKILL.md";
    auto pair = InMemoryTransport::CreatePair();
    auto server = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::vector<SkillResource> manifest{
        SkillResource{uri, TestDigest("x"), 1}};
    server->SetRequestHandler(methods::kGetSkill,
        [&](const JsonRpcRequest&, std::promise<JsonValue> p) {
            p.set_value(TestSkillGetResult(TestSkillValue(uri, "code-review", manifest)));
        });
    server->SetRequestHandler(methods::kReadResource,
        [&](const JsonRpcRequest&, std::promise<JsonValue> p) {
            p.set_value(TestReadResourceResult(uri, "blob", "not-base64!"));
        });
    server->Start();
    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.enable_skills_extension = true;
    auto client = McpClient::Create(std::move(pair.client), opts);

    const auto result = client->ReadSkill(uri);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].reason, "invalid_encoding");
    EXPECT_TRUE(result.files.empty());
    client->Close();
    server->Close();
}

TEST(SkillSafetyTest, ClientFrontmatterOnlyDoesNotExpandOtherFiles) {
    const std::string body = "---\nname: code-review\n---\nbody\n";
    const std::string uri = "skill://code-review/SKILL.md";
    const std::string extra = "skill://code-review/extra.txt";
    auto pair = InMemoryTransport::CreatePair();
    auto server = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> reads{0};
    std::vector<SkillResource> manifest{
        SkillResource{uri, TestDigest(body), static_cast<int64_t>(body.size())},
        SkillResource{extra, TestDigest("extra"), 5}};
    server->SetRequestHandler(methods::kGetSkill,
        [&](const JsonRpcRequest&, std::promise<JsonValue> p) {
            p.set_value(TestSkillGetResult(TestSkillValue(uri, "code-review", manifest)));
        });
    server->SetRequestHandler(methods::kReadResource,
        [&](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            reads.fetch_add(1);
            const std::string requested = req.params && req.params->IsObject()
                ? (*req.params)["uri"].GetString() : std::string();
            p.set_value(TestReadResourceResult(requested, "text",
                requested == uri ? body : "extra"));
        });
    server->Start();
    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.enable_skills_extension = true;
    auto client = McpClient::Create(std::move(pair.client), opts);

    const auto result = client->ReadSkill(uri, {}, SkillReadParts::FrontmatterOnly);
    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_TRUE(result.files.empty());
    EXPECT_EQ(reads.load(), 1);
    client->Close();
    server->Close();
}

TEST(SkillSafetyTest, LegacyClientDoesNotSendModernSkillsRequests) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);
    ClientOptions opts;
    opts.connect_mode = ConnectMode::Legacy;
    opts.initialization_timeout = std::chrono::seconds(5);
    opts.enable_skills_extension = true;
    auto client = McpClient::Create(std::move(pair.client), opts);
    ASSERT_FALSE(client->IsModernProtocol());
    EXPECT_EQ(ResolveSkillRoute(client->GetNegotiatedProtocolVersion(),
        client->GetServerCapabilities()), SkillRoute::Legacy);
    EXPECT_THROW(client->ListSkills(), McpError);
    client->Close();
    server->Close();
}
