#pragma once
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/client/ClientOptions.hpp>

#include <memory>

namespace mcp {

struct NegotiationResult {
    bool is_modern;
    std::string negotiated_version;
    std::optional<DiscoverResult> discover;
    std::optional<InitializeResult> initialize;
    ServerCapabilities capabilities;
    Implementation server_info;
    std::optional<std::string> instructions;
};

class VersionNegotiation {
public:
    static NegotiationResult Negotiate(
        McpSessionHandler& handler,
        const ClientOptions& options);

    static std::optional<DiscoverResult> ProbeDiscover(
        McpSessionHandler& handler,
        std::string_view preferred_version,
        std::chrono::seconds timeout,
        const ClientOptions& options);

    static InitializeResult HandshakeInitialize(
        McpSessionHandler& handler,
        const Implementation& client_info,
        const std::optional<ClientCapabilities>& capabilities,
        std::chrono::seconds timeout);
};

} // namespace mcp
