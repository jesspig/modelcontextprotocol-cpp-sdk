#pragma once

#include <mcp/Capabilities.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/ProtocolVersion.hpp>

#include <optional>
#include <string>
#include <variant>

namespace mcp {

using ProgressToken = std::variant<std::string, int64_t>;

enum class LoggingLevel {
    Debug, Info, Notice, Warning, Error, Critical, Alert, Emergency
};

// ═══════════════════════════════════════════════════════════════════════
// RequestMeta
// ═══════════════════════════════════════════════════════════════════════
struct RequestMeta {
    std::optional<ProgressToken> progress_token;
    std::string protocol_version{kLatestProtocolVersion.data()};
    std::optional<Implementation> client_info;
    std::optional<ClientCapabilities> client_capabilities;
    std::optional<LoggingLevel> log_level;
    std::optional<JsonValue> extensions;
};

// ═══════════════════════════════════════════════════════════════════════
// CacheHint
// ═══════════════════════════════════════════════════════════════════════
struct CacheHint {
    std::optional<int64_t> ttl_ms;
    std::optional<std::string> cache_scope;
};

} // namespace mcp
