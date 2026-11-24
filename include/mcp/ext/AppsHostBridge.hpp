#pragma once

#include <mcp/JsonValue.hpp>
#include <mcp/ext/Apps.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::ext {

inline constexpr std::string_view kAppsMessageSchemaVersion = "2026-07-28";
inline constexpr std::size_t kAppsCapabilityTokenBytes = 32;
inline constexpr std::chrono::milliseconds kDefaultAppsCapabilityTtl{60000};

struct AppsHostBridgeOptions {
    UiMeta ui_meta;
    std::vector<std::string> allowed_origins;
    std::vector<std::string> host_allowed_methods;
    std::size_t max_message_bytes = 65536;
    std::size_t max_depth = 32;
    std::size_t max_nodes = 10000;
    std::size_t max_string_bytes = 65536;
    std::size_t max_id_bytes = 128;
    std::size_t max_method_bytes = 128;
    std::chrono::milliseconds capability_ttl{kDefaultAppsCapabilityTtl};
    bool single_use_sensitive_methods = true;
};

struct AppsPostMessage {
    std::string schema_version{kAppsMessageSchemaVersion};
    std::optional<std::string> request_id;
    std::string method;
    JsonValue params;
};

struct AppsMessageContext {
    std::string source_origin;
    std::string target_origin;
    std::string resource_uri;
    bool sandbox_verified = false;
    std::string capability;
};

struct AppsSandboxPolicy {
    bool opaque_origin = true;
    bool disallow_allow_same_origin = true;
    bool disallow_top_navigation = true;
    bool disallow_popups = true;
    bool disallow_forms = true;
    bool disallow_downloads = true;
    std::vector<std::string> csp;
};

class AppsHostBridge {
public:
    static std::unique_ptr<AppsHostBridge> Create(AppsHostBridgeOptions options);
    ~AppsHostBridge();

    AppsHostBridge(const AppsHostBridge&) = delete;
    AppsHostBridge& operator=(const AppsHostBridge&) = delete;

    std::string SerializeHostMessage(const AppsPostMessage& message,
                                     const AppsMessageContext& context) const;
    AppsPostMessage ParseAppMessage(std::string_view payload,
                                    const AppsMessageContext& context) const;
    [[nodiscard]] bool IsMethodAllowed(std::string_view method,
                                       const AppsMessageContext& context) const;
    std::string CreateCapability(const AppsMessageContext& context) const;
    [[nodiscard]] AppsSandboxPolicy GetSandboxPolicy() const;

private:
    struct Impl;
    explicit AppsHostBridge(std::unique_ptr<Impl> impl);
    void ValidateTrustedContext(const AppsMessageContext& context) const;
    bool CheckCapability(const AppsMessageContext& context, std::string_view method,
                         std::size_t payload_bytes, bool consume) const;
    std::unique_ptr<Impl> impl_;
};

bool NormalizeOrigin(std::string_view origin, std::string& normalized);
bool IsAppsSensitiveMethod(std::string_view method);

} // namespace mcp::ext
