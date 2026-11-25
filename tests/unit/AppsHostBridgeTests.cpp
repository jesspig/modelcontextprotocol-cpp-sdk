#include <mcp/ext/AppsHostBridge.hpp>
#include <mcp/McpError.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using mcp::JsonValue;
using mcp::McpError;
using namespace mcp::ext;

AppsHostBridgeOptions MakeOptions() {
    AppsHostBridgeOptions options;
    options.ui_meta.resource_uri = "ui://server/app";
    options.ui_meta.csp = {"default-src 'none'", "frame-src 'none'"};
    options.ui_meta.permissions = {"ui/initialize", "tools/call"};
    options.host_allowed_methods = {"ui/initialize", "tools/call"};
    options.allowed_origins = {"https://app.example.com", "https://host.example.com:443"};
    options.max_message_bytes = 65536;
    options.max_depth = 32;
    options.max_nodes = 10000;
    options.max_string_bytes = 4096;
    options.max_id_bytes = 128;
    options.max_method_bytes = 128;
    options.capability_ttl = std::chrono::seconds(60);
    options.single_use_sensitive_methods = true;
    return options;
}

std::string MakePayload(std::string_view method = "ui/initialize",
                        std::string_view params = "{}",
                        std::optional<std::string> id = std::nullopt,
                        std::string_view version = "2026-07-28") {
    std::string out = "{\"schema_version\":\"";
    out.append(version);
    out.append("\",\"method\":\"");
    out.append(method);
    out.append("\",\"params\":");
    out.append(params);
    if (id.has_value()) {
        out.append(",\"id\":\"");
        out.append(*id);
        out.append("\"");
    }
    out.append("}");
    return out;
}

AppsMessageContext MakeContext(const AppsHostBridge& bridge, bool sandbox_verified = true) {
    AppsMessageContext context;
    context.source_origin = "https://app.example.com";
    context.target_origin = "https://host.example.com";
    context.resource_uri = "ui://server/app";
    context.sandbox_verified = sandbox_verified;
    if (sandbox_verified) {
        context.capability = bridge.CreateCapability(context);
    }
    return context;
}

TEST(AppsHostBridgeTest, RoundTripSerializeParse) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    const AppsMessageContext context = MakeContext(*bridge);
    AppsPostMessage outgoing;
    outgoing.method = "ui/initialize";
    outgoing.request_id = "req-1";
    JsonValue::Object params;
    params["level"] = JsonValue("basic");
    outgoing.params = JsonValue(std::move(params));
    const std::string wire = bridge->SerializeHostMessage(outgoing, context);
    const AppsPostMessage incoming = bridge->ParseAppMessage(wire, context);
    EXPECT_EQ(incoming.schema_version, std::string(kAppsMessageSchemaVersion));
    EXPECT_EQ(incoming.method, "ui/initialize");
    ASSERT_TRUE(incoming.request_id.has_value());
    EXPECT_EQ(*incoming.request_id, "req-1");
    ASSERT_TRUE(incoming.params.IsObject());
    EXPECT_EQ(incoming.params["level"].GetString(), "basic");
}

TEST(AppsHostBridgeTest, OriginNormalizationExactComparison) {
    std::string normalized;
    EXPECT_TRUE(NormalizeOrigin("HTTPS://Example.COM:443", normalized));
    EXPECT_EQ(normalized, "https://example.com:443");
    EXPECT_TRUE(NormalizeOrigin("http://localhost:3000", normalized));
    EXPECT_EQ(normalized, "http://localhost:3000");
    EXPECT_FALSE(NormalizeOrigin("*", normalized));
    EXPECT_FALSE(NormalizeOrigin("null", normalized));
    EXPECT_FALSE(NormalizeOrigin("", normalized));
    EXPECT_FALSE(NormalizeOrigin("https://user@app.example.com", normalized));
    EXPECT_FALSE(NormalizeOrigin("https://app.example.com/path", normalized));
    EXPECT_FALSE(NormalizeOrigin("ftp://app.example.com", normalized));
    EXPECT_FALSE(NormalizeOrigin("https://app.example.com:99999", normalized));
}

TEST(AppsHostBridgeTest, PayloadOriginForgeryRejected) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    const AppsMessageContext context = MakeContext(*bridge);
    const std::string payload =
        "{\"schema_version\":\"2026-07-28\",\"method\":\"ui/initialize\",\"params\":{},"
        "\"source_origin\":\"https://evil.example\",\"target_origin\":\"*\"}";
    EXPECT_THROW(bridge->ParseAppMessage(payload, context), McpError);
    try {
        (void)bridge->ParseAppMessage(payload, context);
        FAIL() << "forged origin fields must be rejected";
    } catch (const McpError& error) {
        EXPECT_EQ(std::string(error.ErrorMessage()), "apps bridge: unknown message field");
    }
}

TEST(AppsHostBridgeTest, TargetWildcardRejected) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    AppsMessageContext context = MakeContext(*bridge);
    context.target_origin = "*";
    EXPECT_THROW(bridge->CreateCapability(context), McpError);
    EXPECT_THROW(bridge->ParseAppMessage(MakePayload(), context), McpError);
    context.target_origin = "https://host.example.com";
    EXPECT_NO_THROW(bridge->ParseAppMessage(MakePayload(), context));
}

TEST(AppsHostBridgeTest, CrossOriginRejectedButCanonicalAliasAccepted) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    AppsMessageContext context = MakeContext(*bridge);
    context.source_origin = "https://evil.example";
    EXPECT_THROW(bridge->ParseAppMessage(MakePayload(), context), McpError);
    context.source_origin = "https://app.example.com:8443";
    EXPECT_THROW(bridge->ParseAppMessage(MakePayload(), context), McpError);
    context.source_origin = "https://APP.EXAMPLE.COM:443";
    EXPECT_NO_THROW(bridge->ParseAppMessage(MakePayload(), context));
}

TEST(AppsHostBridgeTest, SandboxNotVerifiedRejected) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    AppsMessageContext context = MakeContext(*bridge, false);
    EXPECT_THROW(bridge->CreateCapability(context), McpError);
    EXPECT_THROW(bridge->ParseAppMessage(MakePayload(), context), McpError);
    EXPECT_THROW(bridge->SerializeHostMessage(AppsPostMessage{}, context), McpError);
    try {
        (void)bridge->ParseAppMessage(MakePayload(), context);
        FAIL() << "unverified sandbox must fail closed";
    } catch (const McpError& error) {
        EXPECT_EQ(std::string(error.ErrorMessage()), "apps bridge: sandbox not verified");
    }
}

TEST(AppsHostBridgeTest, SandboxPolicyExposesOpaqueAndCspFromUiMeta) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    const AppsSandboxPolicy policy = bridge->GetSandboxPolicy();
    EXPECT_TRUE(policy.opaque_origin);
    EXPECT_TRUE(policy.disallow_allow_same_origin);
    EXPECT_TRUE(policy.disallow_top_navigation);
    EXPECT_TRUE(policy.disallow_popups);
    EXPECT_TRUE(policy.disallow_forms);
    EXPECT_TRUE(policy.disallow_downloads);
    ASSERT_EQ(policy.csp.size(), 2U);
    EXPECT_EQ(policy.csp[0], "default-src 'none'");
    EXPECT_EQ(policy.csp[1], "frame-src 'none'");
}

TEST(AppsHostBridgeTest, PayloadCannotEscalatePermissions) {
    AppsHostBridgeOptions options = MakeOptions();
    options.ui_meta.permissions = {"ui/initialize"};
    const auto bridge = AppsHostBridge::Create(options);
    const AppsMessageContext context = MakeContext(*bridge);
    EXPECT_FALSE(bridge->IsMethodAllowed("tools/call", context));
    EXPECT_THROW(bridge->ParseAppMessage(MakePayload("tools/call"), context), McpError);
    const std::string escalated =
        "{\"schema_version\":\"2026-07-28\",\"method\":\"ui/initialize\",\"params\":{},"
        "\"permissions\":[\"tools/call\"]}";
    EXPECT_THROW(bridge->ParseAppMessage(escalated, context), McpError);
}

TEST(AppsHostBridgeTest, UnknownMethodRejected) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    const AppsMessageContext context = MakeContext(*bridge);
    EXPECT_FALSE(bridge->IsMethodAllowed("ui/unknown", context));
    EXPECT_THROW(bridge->ParseAppMessage(MakePayload("ui/unknown"), context), McpError);
}

TEST(AppsHostBridgeTest, SensitiveMethodRequiresSingleUseCapability) {
    AppsHostBridgeOptions options = MakeOptions();
    options.single_use_sensitive_methods = false;
    const auto reusable = AppsHostBridge::Create(options);
    const AppsMessageContext reusable_context = MakeContext(*reusable);
    EXPECT_FALSE(reusable->IsMethodAllowed("tools/call", reusable_context));
    EXPECT_THROW(reusable->ParseAppMessage(MakePayload("tools/call"), reusable_context), McpError);
    options.single_use_sensitive_methods = true;
    const auto single_use = AppsHostBridge::Create(options);
    const AppsMessageContext single_use_context = MakeContext(*single_use);
    EXPECT_TRUE(single_use->IsMethodAllowed("tools/call", single_use_context));
    EXPECT_NO_THROW(single_use->ParseAppMessage(MakePayload("tools/call"), single_use_context));
}

TEST(AppsHostBridgeTest, CapabilityReplayRejected) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    const AppsMessageContext context = MakeContext(*bridge);
    const std::string payload = MakePayload("ui/initialize");
    EXPECT_NO_THROW(bridge->ParseAppMessage(payload, context));
    try {
        (void)bridge->ParseAppMessage(payload, context);
        FAIL() << "single-use capability replay must be rejected";
    } catch (const McpError& error) {
        EXPECT_EQ(std::string(error.ErrorMessage()), "apps bridge: capability rejected");
    }
}

TEST(AppsHostBridgeTest, ExpiredCapabilityRejected) {
    AppsHostBridgeOptions options = MakeOptions();
    options.capability_ttl = std::chrono::milliseconds(1);
    const auto bridge = AppsHostBridge::Create(options);
    const AppsMessageContext context = MakeContext(*bridge);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_THROW(bridge->ParseAppMessage(MakePayload(), context), McpError);
}

TEST(AppsHostBridgeTest, CapabilityIsUnpredictable) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    const AppsMessageContext first = MakeContext(*bridge);
    const AppsMessageContext second = MakeContext(*bridge);
    EXPECT_EQ(first.capability.size(), kAppsCapabilityTokenBytes * 2);
    EXPECT_NE(first.capability, second.capability);
}

TEST(AppsHostBridgeTest, OversizeMessageRejected) {
    AppsHostBridgeOptions options = MakeOptions();
    options.max_message_bytes = 96;
    const auto bridge = AppsHostBridge::Create(options);
    const AppsMessageContext context = MakeContext(*bridge);
    const std::string payload = MakePayload("ui/initialize", "{\"data\":\"" + std::string(200, 'x') + "\"}");
    try {
        (void)bridge->ParseAppMessage(payload, context);
        FAIL() << "oversize payload must be rejected";
    } catch (const McpError& error) {
        EXPECT_EQ(std::string(error.ErrorMessage()), "apps bridge: message byte budget exceeded");
    }
}

TEST(AppsHostBridgeTest, DeepNestingRejected) {
    AppsHostBridgeOptions options = MakeOptions();
    options.max_depth = 8;
    const auto bridge = AppsHostBridge::Create(options);
    const AppsMessageContext context = MakeContext(*bridge);
    const std::string params = std::string(20, '[') + std::string(20, ']');
    EXPECT_THROW(bridge->ParseAppMessage(MakePayload("ui/initialize", params), context), McpError);
}

TEST(AppsHostBridgeTest, DuplicateKeyRejected) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    const AppsMessageContext context = MakeContext(*bridge);
    const std::string payload =
        "{\"schema_version\":\"2026-07-28\",\"method\":\"ui/initialize\",\"params\":{},"
        "\"method\":\"tools/call\"}";
    try {
        (void)bridge->ParseAppMessage(payload, context);
        FAIL() << "duplicate keys must be rejected";
    } catch (const McpError& error) {
        EXPECT_EQ(std::string(error.ErrorMessage()), "apps bridge: duplicate object key");
    }
}

TEST(AppsHostBridgeTest, FloatingNumbersParsed) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    const AppsMessageContext context = MakeContext(*bridge);
    const auto incoming = bridge->ParseAppMessage(
        MakePayload("ui/initialize", "{\"fraction\":1.25,\"exponent\":2.5e2}"), context);
    ASSERT_TRUE(incoming.params.IsObject());
    EXPECT_DOUBLE_EQ(incoming.params["fraction"].GetDouble(), 1.25);
    EXPECT_DOUBLE_EQ(incoming.params["exponent"].GetDouble(), 250.0);
}

TEST(AppsHostBridgeTest, NonFiniteNumberRejected) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    const AppsMessageContext context = MakeContext(*bridge);
    EXPECT_THROW(bridge->ParseAppMessage(MakePayload("ui/initialize", "{\"n\":1e999}"), context), McpError);
}

TEST(AppsHostBridgeTest, LongIdAndMethodRejected) {
    AppsHostBridgeOptions options = MakeOptions();
    options.max_id_bytes = 8;
    options.max_method_bytes = 16;
    const auto bridge = AppsHostBridge::Create(options);
    const AppsMessageContext context = MakeContext(*bridge);
    EXPECT_THROW(bridge->ParseAppMessage(MakePayload("ui/initialize", "{}", "123456789"), context), McpError);
    EXPECT_THROW(bridge->ParseAppMessage(MakePayload("ui/veryverylongmethod"), context), McpError);
}

TEST(AppsHostBridgeTest, MalformedSchemaVersionRejected) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    const AppsMessageContext context = MakeContext(*bridge);
    EXPECT_THROW(bridge->ParseAppMessage(MakePayload("ui/initialize", "{}", std::nullopt, "v1"), context), McpError);
    EXPECT_THROW(bridge->ParseAppMessage(MakePayload("ui/initialize", "{}", std::nullopt, ""), context), McpError);
}

TEST(AppsHostBridgeTest, ConcurrentRequestsAreIsolated) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    std::atomic<int> successes{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 16; ++index) {
        threads.emplace_back([&, index] {
            const AppsMessageContext context = MakeContext(*bridge);
            const std::string id = "req-" + std::to_string(index);
            const AppsPostMessage message =
                bridge->ParseAppMessage(MakePayload("ui/initialize", "{}", id), context);
            if (message.request_id == id) {
                ++successes;
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(successes.load(), 16);
}

TEST(AppsHostBridgeTest, ConcurrentReplaySucceedsOnce) {
    const auto bridge = AppsHostBridge::Create(MakeOptions());
    const AppsMessageContext context = MakeContext(*bridge);
    const std::string payload = MakePayload("ui/initialize");
    std::atomic<int> successes{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 16; ++index) {
        threads.emplace_back([&] {
            try {
                (void)bridge->ParseAppMessage(payload, context);
                ++successes;
            } catch (const McpError&) {
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(successes.load(), 1);
}

TEST(AppsHostBridgeTest, ExistingAppsApiUnchanged) {
    EXPECT_EQ(std::string(mcp::ext::kAppsExtensionId), "io.modelcontextprotocol/ui");
    EXPECT_EQ(std::string(mcp::ext::kAppsMimeType), "text/html;profile=mcp-app");
    EXPECT_EQ(std::string(mcp::ext::kAppsUriScheme), "ui://");
    UiMeta meta;
    meta.resource_uri = "ui://server/app";
    EXPECT_NO_THROW(ValidateUiMeta(meta));
    meta.resource_uri = "https://server/app";
    EXPECT_THROW(ValidateUiMeta(meta), McpError);
}

} // namespace
