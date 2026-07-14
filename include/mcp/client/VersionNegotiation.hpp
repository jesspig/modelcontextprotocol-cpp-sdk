#pragma once

#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/client/ClientOptions.hpp>

#include <memory>

namespace mcp {

// ── Version negotiation result ──
struct NegotiationResult {
    bool is_modern;  // true = 2026-era (stateless), false = legacy
    std::string negotiated_version;
    std::optional<DiscoverResult> discover;     // populated if modern
    std::optional<InitializeResult> initialize; // populated if legacy
    ServerCapabilities capabilities;
    Implementation server_info;
    std::optional<std::string> instructions;
};

// ── VersionNegotiation — auto-detect server era ──
// 1. Send server/discover
// 2. On success → modern era (2026-07-28)
// 3. On -32022 / -32601 / timeout → fall back to initialize handshake
class VersionNegotiation {
public:
    // Probe the server and negotiate the best protocol version.
    // Must be called after connecting the transport but before sending
    // non-negotiation requests.
    static NegotiationResult Negotiate(
        McpSessionHandler& handler,
        const ClientOptions& options);

    // Send discover probe
    static std::optional<DiscoverResult> ProbeDiscover(
        McpSessionHandler& handler,
        std::string_view preferred_version,
        std::chrono::seconds timeout);

    // Send initialize handshake
    static InitializeResult HandshakeInitialize(
        McpSessionHandler& handler,
        const Implementation& client_info,
        const std::optional<ClientCapabilities>& capabilities,
        std::chrono::seconds timeout);
};

} // namespace mcp
